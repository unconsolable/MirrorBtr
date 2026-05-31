#pragma once

#include <byteswap.h>
#include "btr0btr.h"
#include "btr0pcur.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "rem0rec.h"
#include "page0page.h"

namespace shadow {

// Read a fixed-length field as big-endian bytes and return as host-order uint64_t.
// Caller must ensure 1 <= field_len <= 8.
inline ulint ReadFieldBE(const byte *data, ulint field_len) {
  ulint key = 0;
  // Copy bytes into the low bits of key, preserving big-endian order.
  // InnoDB stores index fields in big-endian, so this preserves sort order.
  for (ulint i = 0; i < field_len; i++) {
    key = (key << 8) | data[i];
  }
  return key;
}

// Compute the total fixed byte length of the leading n_unique_in_tree fields
// used as the composite key for the shadow index.  Returns 0 if any of those
// fields is variable-length or the total exceeds 8 bytes.
inline ulint GetCompositeKeyLen(const dict_index_t *index) {
  ulint n_unique = dict_index_get_n_unique_in_tree(index);
  ulint total = 0;
  for (ulint i = 0; i < n_unique; i++) {
    const dict_field_t *field = index->get_field(i);
    if (field->prefix_len > 0 || field->fixed_len == 0) return 0;
    total += field->fixed_len;
  }
  return total <= 8 ? total : 0;
}

// Encode the leading n_unique_in_tree fields of a record into a single uint64_t.
inline ulint GetRecordKey(const dict_index_t *index, const rec_t *rec) {
  Rec_offsets rec_offsets;
  const ulint *offsets = rec_offsets.compute(rec, index);
  ulint n_unique = dict_index_get_n_unique_in_tree(index);
  ulint key = 0;
  for (ulint i = 0; i < n_unique; i++) {
    ulint field_len = 0;
    const byte *data = rec_get_nth_field_instant(rec, offsets, i, index, &field_len);
    key = (key << (field_len * 8)) | ReadFieldBE(data, field_len);
  }
  return key;
}

// Encode a dtuple into a single uint64_t using the same key space as GetRecordKey.
// For secondary indexes, the search tuple may have fewer fields than n_unique_in_tree;
// missing fields are padded with 0 bytes (minimum value).
//
// Example: secondary index k_1(k) with PK (id), query "k BETWEEN 100 AND 200".
//   - dtuple only has field [k=100], no id (range scan doesn't need a specific id)
//   - n_unique_in_tree = 2 (k + id), n_tuple = 1
//   - Encoded key = (100 << 32) | 0 → lands on the first leaf page containing k=100
inline ulint GetDtupleKey(const dtuple_t *tuple, const dict_index_t *index) {
  ulint n_tuple = dtuple_get_n_fields(tuple);
  ulint n_unique = dict_index_get_n_unique_in_tree(index);
  ulint key = 0;
  for (ulint i = 0; i < n_unique; i++) {
    if (i < n_tuple) {
      const dfield_t *field = dtuple_get_nth_field(tuple, i);
      const byte *data = static_cast<const byte *>(dfield_get_data(field));
      ulint len = dfield_get_len(field);
      key = (key << (len * 8)) | ReadFieldBE(data, len);
    } else {
      // Pad missing fields with 0 (minimum value for that field position)
      ulint field_len = index->get_field(i)->fixed_len;
      key <<= (field_len * 8);
    }
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

/** Ensure the shadow (learned index) is initialized for the given index.
    Must be called before using shadow traversal or building linear models.
    Returns true if the shadow is built and applicable, false otherwise. */
inline bool BtrEnsureShadow(dict_index_t *index) {
  if (!btr_search_enabled || !index->shadow.IsApplicable()) {
    return false;
  }
  if (index->shadow.IsShadowBuild()) {
    return true;
  }

  auto &build_mutex = index->shadow.BuildMutex();
  std::unique_lock lock(build_mutex);

  if (!index->shadow.IsApplicable()) {
    return false;
  }
  if (index->shadow.IsShadowBuild()) {
    return true;
  }

  if (shadow::GetCompositeKeyLen(index) == 0) {
    index->shadow.SetApplicable(false);
    return false;
  }

  mtr_t iter_mtr;
  mtr_start(&iter_mtr);
  mtr_sx_lock(dict_index_get_lock(index), &iter_mtr, UT_LOCATION_HERE);

  ulint root_level = btr_height_get(index, &iter_mtr);

  // only support height > 1
  if (root_level == 0) {
    index->shadow.SetApplicable(false);
    mtr_memo_release(&iter_mtr, dict_index_get_lock(index), MTR_MEMO_SX_LOCK);
    mtr_commit(&iter_mtr);
    return false;
  }

  // iterate on leaf nodes, to get the <min_key, page id> pairs
  // build_linear_model=false to avoid recursion (we are inside BtrEnsureShadow)
  btr_pcur_t pcur;
  pcur.open_at_side(true, index, BTR_SEARCH_TREE | BTR_ALREADY_S_LATCHED, true, 1, &iter_mtr, false);

  Rec_offsets iter_rec_offsets;
  dberr_t iter_err = DB_SUCCESS;
  std::vector<std::pair<ulint, page_no_t>> key_page_pairs;

  while (iter_err == DB_SUCCESS) {
    if (pcur.is_before_first_on_page()) {
      pcur.move_to_next_on_page();
    }
    rec_t *rec = pcur.get_rec();

    ulint key = shadow::GetRecordKey(index, rec);
    const ulint *iter_offsets = iter_rec_offsets.compute(rec, index);
    uint32_t page_no = btr_node_ptr_get_child_page_no(rec, iter_offsets);

    key_page_pairs.emplace_back(key, page_no);
    iter_err = pcur.move_to_next_user_rec(&iter_mtr);
  }
  btr_leaf_page_release(pcur.get_block(), BTR_SEARCH_LEAF, &iter_mtr);
  pcur.close();

  {
    // Get the minimum key on the tree, sometime the minimum key from parent is wrong.
    pcur.open_at_side(true, index, BTR_SEARCH_TREE | BTR_ALREADY_S_LATCHED, false, 0, &iter_mtr, false);
    if (pcur.is_before_first_on_page()) {
      pcur.move_to_next_on_page();
    }
    rec_t *rec = pcur.get_rec();

    ulint key = shadow::GetRecordKey(index, rec);

    if (key_page_pairs[0].first != key) {
      std::cerr << "Update first key: " << key_page_pairs[0].first << " to " << key << '\n';
      key_page_pairs[0].first = key;
    }
    pcur.close();
  }

  if (index->shadow.IsApplicable()) {
    index->shadow.BuildShadow(key_page_pairs.data(), key_page_pairs.size());
    index->shadow.PrintStat();
  }

  mtr_memo_release(&iter_mtr, dict_index_get_lock(index), MTR_MEMO_SX_LOCK);
  mtr_commit(&iter_mtr);

  return index->shadow.IsShadowBuild();
}

}  // namespace shadow