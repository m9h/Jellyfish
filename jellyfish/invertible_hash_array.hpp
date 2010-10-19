#ifndef __JELLYFISH_REVERSIBLE_HASH__
#define __JELLYFISH_REVERSIBLE_HASH__

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <iostream>
#include <utility>
#include <exception>
#include <assert.h>
#include <jellyfish/storage.hpp>
#include <jellyfish/misc.hpp>
#include <jellyfish/square_binary_matrix.hpp>
#include <jellyfish/storage.hpp>
#include <jellyfish/offsets_key_value.hpp>

namespace jellyfish {
  namespace invertible_hash {
    class InvalidMap : public std::exception {
      virtual const char* what() const throw() {
        return "Mapped region is invalid";
      }
    };

    /* (key,value) pair bit-packed array.  It implements the logic of the
     * packed hash except for size doubling. Setting or incrementing a key
     * will return false if the hash is full. No memory management is done
     * in this class either.
     *
     * The hash function is assumed to be invertible. The key is not
     * directly stored in the hash. Let h = hash(key), size_table =
     * 2**k and key_len = h+k. In the key field is written
     *
     * h high bits of h, reprobe value + 1
     *
     * The +1 for the reprobe value is so that the field is guaranteed
     * not to be zero.
     */
    template <typename word, typename atomic_t, typename mem_block_t>
    class array : public storage_t {
      typedef typename Offsets<word>::offset_t offset_t;
      uint_t             lsize;    // log of size
      size_t             size, size_mask;
      uint_t             reprobe_limit;
      uint_t             lreprobe_limit;
      uint_t             key_len;  // original key len
      word               key_mask; // mask for high bits of hash(key)
      uint_t             key_off;  // offset in key field for reprobe value
      Offsets<word>      offsets;  // key len reduced by size of hash array
      mem_block_t        mem_block;
      word              *data;
      atomic_t           atomic;
      size_t            *reprobes;
      SquareBinaryMatrix hash_matrix;
      SquareBinaryMatrix hash_inverse_matrix;
      struct header {
        uint64_t size;
        uint64_t klen;
        uint64_t clen;
        uint64_t reprobe_limit;
      };

    public:
      typedef word key_t;
      typedef word val_t;

      array(size_t _size, uint_t _key_len, uint_t _val_len,
            uint_t _reprobe_limit, size_t *_reprobes) :
        lsize(ceilLog2(_size)), size(((size_t)1) << lsize), size_mask(size - 1),
        reprobe_limit(_reprobe_limit), key_len(_key_len),
        key_mask(key_len <= lsize ? 0 : (((word)1) << (key_len - lsize)) - 1),
        key_off(key_len <= lsize ? 0 : key_len - lsize),
        offsets(key_off + bitsize(_reprobe_limit + 1), _val_len,
                _reprobe_limit + 1),
        mem_block(div_ceil(size, (size_t)offsets.get_block_len()) * offsets.get_block_word_len() * sizeof(word)),
        data((word *)mem_block.get_ptr()), reprobes(_reprobes),
        hash_matrix(key_len), 
        hash_inverse_matrix(hash_matrix.init_random_inverse())
      {
        if(!data) {
          // TODO: should throw an error
          std::cerr << "allocation failed";
        }
      }
      
      array(char *map, size_t length) :
        hash_matrix(0), hash_inverse_matrix(0) {
        if(length < sizeof(struct header))
          throw new InvalidMap();
        struct header *header = (struct header *)map;
        // TODO: Should make more consistency check on the map...
        size = header->size;
        lsize = ceilLog2(size);
        size_mask = size - 1;
        reprobe_limit = header->reprobe_limit;
        key_len = header->klen;
        offsets.init(key_len + bitsize(reprobe_limit + 1) - lsize, header->clen,
                     reprobe_limit);
        key_mask = (((word)1) << (key_len - lsize)) - 1;
        key_off = key_len - lsize;
        map += sizeof(struct header);
        reprobes = new size_t[header->reprobe_limit + 1];
        memcpy(reprobes, map, sizeof(size_t) * (header->reprobe_limit + 1));
        map += sizeof(size_t) * (header->reprobe_limit + 1);
        map += hash_matrix.read(map);
        map += hash_inverse_matrix.read(map);
        if((size_t)map & 0x7)
          map += 0x8 - ((size_t)map & 0x7); // Make sure aligned for 64bits word. TODO: use alignof?
        data = (word *)map;
      }

      ~array() { }

      size_t get_size() const { return size; }
      uint_t get_key_len() const { return key_len; }
      uint_t get_val_len() const { return offsets.get_val_len(); }
      
      size_t get_max_reprobe_offset() const { 
        return reprobes[reprobe_limit]; 
      }

      size_t floor_block(size_t entries, size_t &blocks) const {
	return offsets.floor_block(entries, blocks);
      }

      /**
       * Zero out blocks in [start, start+length), where start and
       * length are given in number of blocks.
       **/
      void zero_blocks(size_t start, size_t length) {
	char *start_ptr = (char *)(data + start * offsets.get_block_word_len());
	char *end_ptr = (char *)mem_block.get_ptr() + mem_block.get_size();

	if(start_ptr >= end_ptr)
	  return;
	length *= offsets.get_block_word_len() * sizeof(word);
	if(start_ptr + length > end_ptr)
	  length = end_ptr - start_ptr;

	memset(start_ptr, '\0', length);
      }

      // Iterator
      class iterator {
      protected:
        const array  *ary;
        size_t  start_id, nid, end_id;
        uint64_t mask;
      public:
        word     key;
        word     val;
        size_t   id;
        uint64_t hash;

        iterator(const array *_ary, size_t start, size_t end) :
          ary(_ary), start_id(start), nid(start), 
	  end_id(end > ary->get_size() ? ary->get_size() : end),
          mask(ary->get_size() - 1)
        {}
        
        void get_string(char *out) const {
          tostring(key, ary->get_key_len() / 2, out);
        }
        uint64_t get_hash() const { return hash; }
        uint64_t get_pos() const { return hash & mask; }
	uint64_t get_start() const { return start_id; }
	uint64_t get_end() const { return end_id; }

        bool next() {
          bool success;
          while((id = nid) < end_id) {
            nid++;
            success = ary->get_key_val_full(id, key, val);
            if(success) {
              hash = (key & ary->key_mask) << ary->lsize;
              uint_t reprobep = (key >> ary->key_off) - 1;
              hash |= (id - (reprobep > 0 ? ary->reprobes[reprobep] : 0)) & ary->size_mask;
              key = ary->hash_inverse_matrix.times(hash);
              return true;
            }
          }
          return false;
        }
      };
      friend class iterator;
      iterator iterator_all() const { return iterator(this, 0, get_size()); }
      iterator iterator_slice(size_t slice_number, size_t number_of_slice) const {
        size_t slice_size = get_size() / number_of_slice;
        return iterator(this, slice_number * slice_size, (slice_number + 1) * slice_size);
      }

      /* Why on earth doesn't inheritance with : public iterator work
         here? Resort to copying code. Arrrgggg....
       */
      class overlap_iterator {
      protected:
        const array *ary;
        uint64_t     mask;
        size_t       start_id, end_id, start_oid;
        size_t       moid, oid;
      public:
        word     key;
        word     val;
        size_t   id;
        uint64_t hash;

        overlap_iterator(const array *_ary, size_t start, size_t end) :
          ary(_ary),
          mask(ary->get_size() - 1),
          start_id(start),
          end_id(end > ary->get_size() ? ary->get_size() : end),
          start_oid(start),
          moid(end_id - start_id + ary->get_max_reprobe_offset()),
          oid(0)
        {
          // Adjust for very small arrays and it overlaps with itself
          if(moid > ary->get_size() - start_id) {
            size_t last_id = (start_id + moid) % mask;
            if(last_id > start_id)
              moid -= last_id - start_id - 1;
          }
        }
        
        void get_string(char *out) const {
          tostring(key, ary->get_key_len() / 2, out);
        }
        uint64_t get_hash() const { return hash; }
        uint64_t get_pos() const { return hash & mask; }
	uint64_t get_start() const { return start_id; }
	uint64_t get_end() const { return end_id; }

        bool next() {
          bool success;
          while(oid < moid) {
            id = (start_oid + oid++) & mask;
            success = ary->get_key_val_full(id, key, val);
            if(success) {
              hash = (key & ary->key_mask) << ary->lsize;
              uint_t reprobep = (key >> ary->key_off) - 1;
              hash |= (id - (reprobep > 0 ? ary->reprobes[reprobep] : 0)) & ary->size_mask;
              if(get_pos() < start_id || get_pos() >= end_id)
                continue;
              key = ary->hash_inverse_matrix.times(hash);
              return true;
            }
          }
          return false;
        }
      };
      friend class overlap_iterator;

      /*
       * Return the key and value at position id. If the slot at id is
       * empty, returns false. If the slot at position id has the large
       * bit set, the key is resolved by looking backward in the
       * table. The value returned on the other hand is the value at
       * position id. No summation of the other entries for the key is
       * done.
       */
      bool get_key_val(size_t id, word &key, word &val) {
        word     *w, *kvw, *fw = NULL;
        offset_t *o, *lo, *fo  = NULL;
        bool      large;
        uint_t    overflows;

        overflows = 0;
        while(true) {
          w = offsets.get_word_offset(id, &o, &lo, data);
          kvw = w + o->key.woff;
          key = *kvw;
          large = key & o->key.lb_mask;
          if(large)
            o = lo;
          if(o->key.mask2) {
            key = (key & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
            key |= ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift;
          } else {
            key = (key & o->key.mask1) >> o->key.boff;
          }

          // Save offset and word for value retrieval
          if(!fo) {
            fo = o;
            fw = w;
          }

          if(large) {
            if(key)
              id -= reprobes[key];
            id = (id - reprobes[0]) & size_mask;
            overflows++;
          } else {
            break;
          }
        }
        if(!key)
          return false;

        kvw = fw + fo->val.woff;
        val = ((*kvw) & fo->val.mask1) >> fo->val.boff;
        if(fo->val.mask2) {
          val |= ((*(kvw+1)) & fo->val.mask2) << fo->val.shift;
        }
        if(overflows > 0) {
          val <<= offsets.get_val_len();
          if(--overflows > 0)
            val <<= offsets.get_lval_len() * overflows;
        }
        return true;
      }

      /*
       * Return the key and value at position id. If the slot at id is
       * empty or has the large bit set, returns false. Otherwise,
       * returns the key and the value is the sum of all the entries
       * in the hash table for that key. I.e., the table is search
       * forward for entries with large bit set pointing back to the
       * key at id, and all those values are summed up.
       */
      bool get_key_val_full(size_t id, word &key, word &val) const {
        const offset_t	*o, *lo;
        word		*w, *kvw, nkey, nval;
        uint_t		 reprobe = 0, overflows = 0;
        size_t		 cid;

        w   = offsets.get_word_offset(id, &o, &lo, data);
        kvw = w + o->key.woff;
        key = *kvw;
        if(key & o->key.lb_mask)
          return false;
        if(o->key.mask2) {
          if((key & o->key.sb_mask1) == 0)
            return false;
          key  = (key & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
          key |= ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift; 
        } else {
          key = (key & o->key.mask1) >> o->key.boff;
          if(key == 0)
            return false;
        }

        kvw = w + o->val.woff;
        val = ((*kvw) & o->val.mask1) >> o->val.boff;
        if(o->val.mask2)
          val |= ((*(kvw+1)) & o->val.mask2) << o->val.shift;

        // Resolve value
        reprobe = 0;
        cid = id = (id + reprobes[0]) & size_mask;
        while(reprobe <= reprobe_limit) {
          if(reprobe)
            cid  = (id + reprobes[reprobe]) & size_mask;

          w    = offsets.get_word_offset(cid, &o, &lo, data);
          kvw  = w + o->key.woff;
          nkey = *kvw;
          if(nkey & o->key.lb_mask) {
            if(lo->key.mask2) {
              nkey  = (nkey & lo->key.mask1 & ~lo->key.sb_mask1) >> lo->key.boff;
              nkey |= ((*(kvw+1)) & lo->key.mask2 & ~lo->key.sb_mask2) << lo->key.shift;
            } else {
              nkey = (nkey & lo->key.mask1) >> lo->key.boff;
            }
            if(nkey == reprobe) {
              kvw = w + lo->val.woff;
              nval = ((*kvw) & lo->val.mask1) >> lo->val.boff;
              if(lo->val.mask2)
                nval |= ((*(kvw+1)) & lo->val.mask2) << lo->val.shift;
              nval <<= offsets.get_val_len();
              nval <<= offsets.get_lval_len() * overflows;
              val += nval;

              overflows++;
              reprobe = 0;
              cid = id = (cid + reprobes[0]) & size_mask;
              continue;
            }
          } else {
            if(o->key.mask2) {
              if((nkey & o->key.sb_mask1) == 0)
                return true;
            } else {
              if((nkey & o->key.mask1) == 0)
                return true;
            }
          }

          reprobe++;
        }

        return true;
      }

      bool get_val(size_t id, word key, word &val, bool full = false) const {
        word     *w, *kvw, nkey, nval;
        offset_t *o, *lo;
        uint_t    reprobe = 0;
        size_t    cid = id;
    
        // Find key
        while(true) {
          w   = offsets.get_word_offset(cid, &o, &lo, data);
          kvw = w + o->key.woff;
          nkey = *kvw;
      
          if(!(nkey & o->key.lb_mask)) {
            if(o->key.mask2) {
              nkey = (nkey & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
              nkey |=  ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift;
            } else {
              nkey = (nkey & o->key.mask1) >> o->key.boff;
            }
            if(nkey == key)
              break;
          }
          if(++reprobe > reprobe_limit)
            return false;
          cid = (id + reprobes[reprobe]) & size_mask;
        }

        // Get value
        kvw = w + o->val.woff;
        val = ((*kvw) & o->val.mask1) >> o->val.boff;
        if(o->val.mask2) {
          val |= ((*(kvw+1)) & o->val.mask2) << o->val.shift;
        }

        // Eventually get large values...
        if(full) {
          cid = id = (cid + reprobes[0]) & size_mask;
          reprobe = 0;
          do {
            w   = offsets.get_word_offset(cid, &o, &lo, data);
            kvw = w + o->key.woff;
            nkey = *kvw;
            if(nkey & o->key.lb_mask) {
              if(lo->key.mask2) {
                nkey = (nkey & lo->key.mask1 & ~lo->key.sb_mask1) >> lo->key.boff;
                nkey |=  ((*(kvw+1)) & lo->key.mask2 & ~lo->key.sb_mask2) << lo->key.shift;
              } else {
                nkey = (nkey & lo->key.mask1) >> lo->key.boff;
              }
              if(nkey == reprobe) {
                kvw = w + lo->val.woff;
                nval = ((*kvw) & lo->val.mask1) >> lo->val.boff;
                if(lo->val.mask2)
                  nval |= ((*(kvw+1)) & lo->val.mask2) << lo->val.shift;

                val |= nval << offsets.get_val_len();
                break;
              }
            }

            cid = (id + reprobes[++reprobe]) & size_mask;
          } while(reprobe <= reprobe_limit);
        }

        return true;
      }
  
      inline bool add(word key, word val) {
        uint64_t hash = hash_matrix.times(key);
        return add_rec(hash & size_mask, (hash >> lsize) & key_mask,
                       val, false);
      }

      bool add_rec(size_t id, word key, word val, bool large) {
        uint_t		 reprobe     = 0;
        const offset_t	*o, *lo, *ao;
        word		*w, *kw, *vw, nkey;
        bool		 key_claimed = false;
        size_t		 cid         = id;
        word		 cary;
        word		 akey        = large ? 0 : (key | (1 << key_off));

        // Claim key
        do {
          w = offsets.get_word_offset(cid, &o, &lo, data);
          ao = large ? lo : o;

          kw = w + ao->key.woff;

          if(ao->key.mask2) { // key split on two words
            nkey = akey << ao->key.boff;
            nkey |= ao->key.sb_mask1;
            if(large)
              nkey |= ao->key.lb_mask;
            nkey &= ao->key.mask1;

            // Use o->key.mask1 and not ao->key.mask1 as the first one is
            // guaranteed to be bigger. The key needs to be free on its
            // longer mask to claim it!
            key_claimed = set_key(kw, nkey, o->key.mask1, ao->key.mask1);
            if(key_claimed) {
              nkey = ((akey >> ao->key.shift) | ao->key.sb_mask2) & ao->key.mask2;
              key_claimed = key_claimed && set_key(kw + 1, nkey, o->key.mask2, ao->key.mask2);
            }
          } else { // key on one word
            nkey = akey << ao->key.boff;
            if(large)
              nkey |= ao->key.lb_mask;
            nkey &= ao->key.mask1;
            key_claimed = set_key(kw, nkey, o->key.mask1, ao->key.mask1);
          }
          if(!key_claimed) { // reprobe
            if(++reprobe > reprobe_limit)
              return false;
            cid = (id + reprobes[reprobe]) & size_mask;

            if(large)
              akey = reprobe;
            else
              akey = key | ((reprobe + 1) << key_off);
          }
        } while(!key_claimed);

        // Increment value
        vw = w + ao->val.woff;
        cary = add_val(vw, val, ao->val.boff, ao->val.mask1);
        cary >>= ao->val.shift;
        if(cary && ao->val.mask2) { // value split on two words
          cary = add_val(vw + 1, cary, 0, ao->val.mask2);
          cary >>= ao->val.cshift;
        }
        if(cary) {
          cid = (cid + reprobes[0]) & size_mask;
          if(add_rec(cid, key, cary, true))
            return true;

          // Adding failed, table is full. Need to back-track and
          // substract val.
          cary = add_val(vw, (1 << offsets.get_val_len()) - val,
                         ao->val.boff, ao->val.mask1);
          cary >>= ao->val.shift;
          if(cary && ao->val.mask2) {
            // Can I ignore the cary here? Table is known to be full, so
            // not much of a choice. But does it leave the table in a
            // consistent state?
            add_val(vw + 1, cary, 0, ao->val.mask2);
          }
          return false;
        }
        return true;
      }

      void write_ary_header(std::ostream *out) const {
        hash_matrix.dump(out);
        hash_inverse_matrix.dump(out);
      }

      void write_raw(std::ostream *out) const {
        struct header header = { size, key_len, offsets.get_val_len(),
                                 reprobe_limit };
        out->write((char *)&header, sizeof(header));
        out->write((char *)reprobes, sizeof(size_t) * (reprobe_limit + 1));
	write_ary_header(out);
        if(out->tellp() & 0x7) { // Make sure aligned
          string padding(0x8 - (out->tellp() & 0x7), '\0');
          out->write(padding.c_str(), padding.size());
        }
        out->write((char *)mem_block.get_ptr(), mem_block.get_size());
      }

    private:
      inline bool set_key(word *w, word nkey, word free_mask, 
                          word equal_mask) {
        word ow = *w, nw, okey;

        okey = ow & free_mask;
        while(okey == 0) { // large bit not set && key is free
          nw = atomic.cas(w, ow, ow | nkey);
          if(nw == ow)
            return true;
          ow = nw;
          okey = ow & free_mask;
        }
        return (ow & equal_mask) == nkey;
      }

      inline word add_val(word *w, word val, uint_t shift, word mask) {
        word now = *w, ow, nw, nval;

        do {
          ow = now;
          nval = ((ow & mask) >> shift) + val;
          nw = (ow & ~mask) | ((nval << shift) & mask);
          now = atomic.cas(w, ow, nw);
        } while(now != ow);

        return nval & (~(mask >> shift));
      }
    };

    /*****/
  }
}

#endif // __REVERSIBLE_HASH__
