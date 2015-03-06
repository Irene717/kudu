// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
// All rights reserved.

#include <vector>

#include "kudu/common/encoded_key.h"
#include "kudu/common/key_encoder.h"

namespace kudu {

using std::string;


EncodedKey::EncodedKey(faststring* data,
                       vector<const void *> *raw_keys,
                       size_t num_key_cols)
  : num_key_cols_(num_key_cols) {
  int len = data->size();
  data_.reset(data->release());
  encoded_key_ = Slice(data_.get(), len);

  DCHECK_LE(raw_keys->size(), num_key_cols);

  raw_keys_.swap(*raw_keys);
}

Status EncodedKey::DecodeEncodedString(const Schema& schema,
                                       Arena* arena,
                                       const Slice& encoded,
                                       gscoped_ptr<EncodedKey>* result) {
  uint8_t* raw_key_buf = static_cast<uint8_t*>(arena->AllocateBytes(schema.key_byte_size()));
  if (PREDICT_FALSE(!raw_key_buf)) {
    return Status::RuntimeError("OOM");
  }

  RETURN_NOT_OK(schema.DecodeRowKey(encoded, raw_key_buf, arena));

  vector<const void*> raw_keys(schema.num_key_columns());
  for (int i = 0; i < schema.num_key_columns(); i++) {
    raw_keys[i] = raw_key_buf + schema.column_offset(i);
  }

  faststring data_copy;
  data_copy.assign_copy(encoded.data(), encoded.size());

  result->reset(new EncodedKey(&data_copy, &raw_keys, schema.num_key_columns()));
  return Status::OK();
}

Status EncodedKey::IncrementEncodedKey(const Schema& tablet_schema, gscoped_ptr<EncodedKey> *key) {
  EncodedKey* key_ptr = key->get();
  DCHECK_EQ(key_ptr->num_key_columns(), tablet_schema.num_key_columns());
  // Make a new builder out of the contents key.
  EncodedKeyBuilder kb(&tablet_schema);
  for (int i = 0; i < tablet_schema.num_key_columns(); i++) {
    kb.AddColumnKey(key_ptr->raw_keys()[i]);
  }
  // Set the key to its next greater successor.
  gscoped_ptr<EncodedKey> successor(kb.BuildSuccessorEncodedKey());
  if (!successor) {
    return Status::IllegalState("No lexicographically greater key exists");
  }
  key->swap(successor);
  return Status::OK();
}

string EncodedKey::Stringify(const Schema &schema) const {
  if (num_key_cols_ == 1) {
    return schema.column(0).Stringify(raw_keys_.front());
  }

  faststring s;
  s.append("(");
  for (int i = 0; i < num_key_cols_; i++) {
    if (i > 0) {
      s.append(",");
    }
    if (i < raw_keys_.size()) {
      s.append(schema.column(i).Stringify(raw_keys_[i]));
    } else {
      s.append("*");
    }
  }
  s.append(")");
  return s.ToString();
}

////////////////////////////////////////////////////////////

EncodedKeyBuilder::EncodedKeyBuilder(const Schema* schema)
 : schema_(schema),
   encoded_key_(schema->key_byte_size()),
   num_key_cols_(schema->num_key_columns()),
   idx_(0) {
}

void EncodedKeyBuilder::Reset() {
  encoded_key_.clear();
  idx_ = 0;
  raw_keys_.clear();
  encoded_key_.reserve(schema_->key_byte_size());
}

void EncodedKeyBuilder::AddColumnKey(const void *raw_key) {
  DCHECK_LT(idx_, num_key_cols_);

  const ColumnSchema &col = schema_->column(idx_);
  DCHECK(!col.is_nullable());

  const TypeInfo* ti = col.type_info();
  bool is_last = idx_ == num_key_cols_ - 1;
  GetKeyEncoder(ti->type()).Encode(raw_key, is_last, &encoded_key_);
  raw_keys_.push_back(raw_key);

  ++idx_;
}

EncodedKey* EncodedKeyBuilder::BuildSuccessorEncodedKey() {
  return encoded_key_.AdvanceToSuccessor() ? BuildEncodedKey() : NULL;
}

EncodedKey *EncodedKeyBuilder::BuildEncodedKey() {
  if (idx_ == 0) {
    return NULL;
  }
  EncodedKey *ret = new EncodedKey(&encoded_key_, &raw_keys_, num_key_cols_);
  idx_ = 0;
  return ret;
}

void EncodedKeyBuilder::AssignCopy(const EncodedKeyBuilder &other) {
  DCHECK_SCHEMA_EQ(*schema_, *other.schema_);

  encoded_key_.assign_copy(other.encoded_key_.data(),
                           other.encoded_key_.length());
  idx_ = other.idx_;
  raw_keys_.assign(other.raw_keys_.begin(), other.raw_keys_.end());
}

string EncodedKey::RangeToString(const EncodedKey* lower, const EncodedKey* upper) {
  string ret;
  if (lower && upper) {
    ret.append("encoded key BETWEEN ");
    ret.append(lower->encoded_key().ToDebugString());
    ret.append(" AND ");
    ret.append(upper->encoded_key().ToDebugString());
    return ret;
  } else if (lower) {
    ret.append("encoded key >= ");
    ret.append(lower->encoded_key().ToDebugString());
    return ret;
  } else if (upper) {
    ret.append("encoded key <= ");
    ret.append(upper->encoded_key().ToDebugString());
  } else {
    LOG(DFATAL) << "Invalid key!";
    ret = "invalid key range";
  }
  return ret;
}

} // namespace kudu
