#pragma once

#include <byteswap.h>
#include "dict0mem.h"
#include "rem0rec.h"
#include "page0page.h"

namespace shadow {

inline ulint GetRecordKey(const dict_index_t *index, const rec_t *rec) {
  Rec_offsets rec_offsets;
  const byte *field_data;
  ulint field_length;
  const ulint *offsets = rec_offsets.compute(rec, index);
  field_data = rec_get_nth_field_instant(rec, offsets, 0, index, &field_length);

  ulint key = 0;
  if (field_length == 8) {
    uint64_t big_endian_key = *(ulint *)(field_data);
    key = bswap_64(big_endian_key);
  } else if (field_length == 4) {
    uint32_t big_endian_key = *(uint32_t *)(field_data);
    key = bswap_32(big_endian_key);
  }

  return key;
}

inline ulint GetDtupleKey(const dtuple_t *tuple) {
  const auto dtuple_field = dtuple_get_nth_field(tuple, 0);
  const auto dtuple_b_ptr =
      static_cast<const byte *>(dfield_get_data(dtuple_field));
  auto dtuple_f_len = dfield_get_len(dtuple_field);
  uint64_t key = 0;

  ut_ad(dtuple_f_len == 4 || dtuple_f_len == 8);
  if (dtuple_f_len == 4) {
    uint32_t big_endian_key = *(uint32_t *)(dtuple_b_ptr);
    key = bswap_32(big_endian_key);
  } else if (dtuple_f_len == 8) {
    uint64_t big_endian_key = *(uint64_t *)(dtuple_b_ptr);
    key = bswap_64(big_endian_key);
  }
  return key;
}

inline void BuildLinearModel(const buf_block_t *block, const dict_index_t *index, const page_t *page) {
  alexol::LinearModel<uint64_t> model;
  alexol::LinearModelBuilder<uint64_t> builder(&model);
  ulint up = page_dir_get_n_slots(page) - 1;

  if (up > 1) {
    for (ulint i = 1; i < up; ++i) {
      const page_dir_slot_t *iter_slot = page_dir_get_nth_slot(page, i);
      const rec_t *iter_rec = page_dir_slot_get_rec(iter_slot);

      // get record key
      ulint key = shadow::GetRecordKey(index, iter_rec);

      // ss << key << " " << i << '\n';
      builder.add(key, static_cast<int>(i));
    }

    builder.build();
    block->model.model_ = model;
    block->model.build_ = true;
  }
}

}  // namespace shadow