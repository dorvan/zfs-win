/* 
 *	Copyright (C) 2010 Gabest
 *	http://code.google.com/p/zfs-win/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "BlockReader.h"
#include "Hash.h"
#include "Compress.h"

namespace ZFS
{
	BlockReader::BlockReader(Pool* pool, blkptr_t* bp, size_t count)
		: m_pool(pool)
	{
		Insert(bp, count);
	}

	BlockReader::~BlockReader()
	{
		for(auto i = m_bpl.begin(); i != m_bpl.end(); i++)
		{
			delete *i;
		}
	}

	void BlockReader::Insert(blkptr_t* bp, size_t count)
	{
		if(m_bpl.empty())
		{
			for(size_t i = 0; i < count; i++)
			{
				if(bp[i].type != DMU_OT_NONE)
				{
					m_bpl.push_back(new blkptr_t(bp[i]));
				}
			}
		}
		else
		{
			std::list<blkptr_t*> l;

			for(size_t i = 0; i < count; i++)
			{
				if(bp[i].type != DMU_OT_NONE)
				{
					l.push_back(new blkptr_t(bp[i]));
				}
			}

			m_bpl.insert(m_bpl.begin(), l.begin(), l.end());
		}
	}

	bool BlockReader::Read(std::vector<uint8_t>& dst, blkptr_t* bp)
	{
		for(int i = 0; i < 3; i++)
		{
			dva_t* addr = &bp->blk_dva[i];

			ASSERT(addr->gang == 0); // TODO: zio_gbh_phys_t (not used ??? never encountered one, yet)

			for(auto i = m_pool->m_vdevs.begin(); i != m_pool->m_vdevs.end(); i++)
			{
				VirtualDevice* vdev = *i;

				if(vdev->id != addr->vdev)
				{
					continue;
				}

				// << 9 or vdev->ashift? 
				//
				// on-disk spec says: "All sizes are stored as the number of 512 byte sectors (minus one) needed to
				// represent the size of this block.", but is it still true or outdated?

				size_t psize = ((size_t)bp->psize + 1) << 9;
				size_t lsize = ((size_t)bp->lsize + 1) << 9;

				std::vector<uint8_t> src(psize);

				vdev->Read(src, psize, addr->offset << 9);

				if(Verify(src, bp->cksum_type, bp->cksum))
				{
					if(Decompress(src, dst, lsize, bp->comp_type))
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	bool BlockReader::Verify(std::vector<uint8_t>& buff, uint8_t cksum_type, cksum_t& cksum)
	{
		cksum_t c;

		memset(&c, 0, sizeof(c));

		switch(cksum_type)
		{
		case ZIO_CHECKSUM_OFF:
			return true;
		case ZIO_CHECKSUM_ON: // ???
		case ZIO_CHECKSUM_ZILOG:
		case ZIO_CHECKSUM_FLETCHER_2:
			fletcher_2_native(buff.data(), buff.size(), &c);
			break;
		case ZIO_CHECKSUM_ZILOG2:
		case ZIO_CHECKSUM_FLETCHER_4:
			fletcher_4_native(buff.data(), buff.size(), &c);
			break;
		case ZIO_CHECKSUM_LABEL:
		case ZIO_CHECKSUM_GANG_HEADER:
		case ZIO_CHECKSUM_SHA256:
			sha256(buff.data(), buff.size(), &c); // TESTME
			break;
		default:
			ASSERT(0);
			return false;
		}

		ASSERT(cksum == c);

		return cksum == c;
	}

	bool BlockReader::Decompress(std::vector<uint8_t>& src, std::vector<uint8_t>& dst, size_t lsize, uint8_t comp_type)
	{
		switch(comp_type)
		{
		case ZIO_COMPRESS_ON: // ???
		case ZIO_COMPRESS_LZJB:
			dst.resize(lsize);
			lzjb_decompress(src.data(), dst.data(), src.size(), lsize);
			break;
		case ZIO_COMPRESS_OFF:
		case ZIO_COMPRESS_EMPTY: // ???
			dst.swap(src);
			break;
		case ZIO_COMPRESS_GZIP_1:
		case ZIO_COMPRESS_GZIP_2:
		case ZIO_COMPRESS_GZIP_3:
		case ZIO_COMPRESS_GZIP_4:
		case ZIO_COMPRESS_GZIP_5:
		case ZIO_COMPRESS_GZIP_6:
		case ZIO_COMPRESS_GZIP_7:
		case ZIO_COMPRESS_GZIP_8:
		case ZIO_COMPRESS_GZIP_9:
			gzip_decompress(src.data(), dst.data(), src.size(), lsize); // TESTME
			break;
		case ZIO_COMPRESS_ZLE:
			zle_decompress(src.data(), dst.data(), src.size(), lsize, 64); // TESTME
			break;
		default:
			ASSERT(0);
			return false;
		}

		return true;
	}

	// BlockStream

	BlockStream::BlockStream(Pool* pool, blkptr_t* bp, size_t count)
		: BlockReader(pool, bp, count)
	{
	}

	BlockStream::~BlockStream()
	{
	}
	
	bool BlockStream::ReadNext(std::vector<uint8_t>& buff)
	{
		if(m_bpl.empty())
		{
			return false;
		}

		std::auto_ptr<blkptr_t> bp(m_bpl.front());

		m_bpl.pop_front();

		while(bp->lvl > 0)
		{
			if(!Read(buff, bp.get()))
			{
				return false;
			}

			Insert((blkptr_t*)buff.data(), buff.size() / sizeof(blkptr_t));

			bp = std::auto_ptr<blkptr_t>(m_bpl.front());

			m_bpl.pop_front();
		}
		
		return Read(buff, bp.get());
	}

	bool BlockStream::ReadToEnd(std::vector<uint8_t>& dst)
	{
		dst.clear();

		// TODO: resize/reserve (how much?)

		size_t i = 0;

		std::vector<uint8_t> buff;

		while(ReadNext(buff))
		{
			if(i + buff.size() > dst.size())
			{
				dst.resize(i + buff.size());
			}

			memcpy(dst.data() + i, buff.data(), buff.size());

			i += buff.size();
		}

		return true;
	}

	// BlockFile

	BlockFile::BlockFile(Pool* pool, blkptr_t* bp, size_t count)
		: BlockReader(pool, bp, count)
		, m_psize(0)
		, m_lsize(0)
	{
		std::list<blkptr_t*> l;

		while(!m_bpl.empty())
		{
			std::auto_ptr<blkptr_t> bp(m_bpl.front());

			m_bpl.pop_front();

			while(bp->lvl > 0)
			{
				std::vector<uint8_t> buff;

				if(!__super::Read(buff, bp.get()))
				{
					ASSERT(0);

					return;
				}

				Insert((blkptr_t*)buff.data(), buff.size() / sizeof(blkptr_t));

				bp = std::auto_ptr<blkptr_t>(m_bpl.front());

				m_bpl.pop_front();
			}

			m_psize += bp.get()->psize + 1;
			m_lsize += bp.get()->lsize + 1;

			l.push_back(bp.release());
		}

		m_psize <<= 9;
		m_lsize <<= 9;

		m_bpl.swap(l);

		m_cache.bp = NULL;
	}

	BlockFile::~BlockFile()
	{
	}
	
	bool BlockFile::Read(void* dst, size_t size, uint64_t offset)
	{
		// TODO: faster than O(n) access to m_bpl 

		uint64_t end = offset + size;

		if(end > m_lsize)
		{
			return false;
		}

		uint8_t* p = (uint8_t*)dst;

		size_t lsize;
		uint64_t lstart = 0;
		uint64_t lnext;

		for(auto i = m_bpl.begin(); i != m_bpl.end(); i++)
		{
			blkptr_t* bp = *i;

			lsize = ((size_t)bp->lsize + 1) << 9;
			lnext = lstart + lsize;

			if(offset < lnext)
			{
				if(m_cache.bp != bp)
				{
					if(!m_pool->Read(m_cache.buff, bp, 1))
					{
						return false;
					}

					m_cache.bp = bp;					
				}

				size_t n = std::min<size_t>(size, (size_t)(lnext - offset));

				memcpy(p, m_cache.buff.data() + (offset - lstart), n);

				p += n;
				size -= n;

				while(size > 0 && ++i != m_bpl.end())
				{
					bp = *i;

					lsize = ((size_t)bp->lsize + 1) << 9;
					lnext = lstart + lsize;

					if(m_cache.bp != bp)
					{
						if(!__super::Read(m_cache.buff, bp))
						{
							return false;
						}

						m_cache.bp = bp;
					}

					size_t n = std::min<size_t>(size, lsize);

					memcpy(p, m_cache.buff.data(), n);

					p += n;
					size -= n;

					lstart = lnext;
				}

				ASSERT(size == 0);

				return size == 0;
			}

			lstart = lnext;
		}

		return false;
	}
}
