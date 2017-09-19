// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_RUNTIME_DECIMAL_VALUE_H
#define IMPALA_RUNTIME_DECIMAL_VALUE_H

#include <ostream>

#include "runtime/multi-precision.h"
#include "runtime/types.h"

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

namespace impala {

/// Implementation of decimal types. The type is parametrized on the underlying
/// storage type, which must implement all operators (example of a storage type
/// is int32_t). The decimal does not store its precision and scale since we'd
/// like to keep the storage as small as possible.
/// Overflow handling: anytime the value is assigned, we need to consider overflow.
/// Overflow is handled by an output return parameter. Functions should set this
/// to true if overflow occured and leave it *unchanged* otherwise (e.g. |= rather than =).
/// This allows the caller to not have to check overflow after every call.
template<typename T>
class DecimalValue {
 public:
  typedef T StorageType;

  DecimalValue() : value_(0) { }
  DecimalValue(const T& s) : value_(s) { }

  DecimalValue& operator=(const T& s) {
    value_ = s;
    return *this;
  }

  /// Returns the closest Decimal to 'd' of type 't', rounding to the nearest integer
  /// if 'round' is true, truncating the decimal places otherwise
  static inline DecimalValue FromDouble(const ColumnType& t, double d, bool round,
      bool* overflow) {
    return FromDouble(t.precision, t.scale, d, round, overflow);
  }

  static inline DecimalValue FromDouble(int precision, int scale, double d,
      bool round, bool* overflow);

  /// Assigns *result as a decimal.
  static inline DecimalValue FromInt(int precision, int scale, int64_t d, bool* overflow);

  /// The overloaded operators assume that this and other have the same scale. They are
  /// more efficient than the comparison functions that handle differing scale and should
  /// be used if the scales are known to be the same.  (e.g. min(decimal_col) or order by
  /// decimal_col.
  bool operator==(const DecimalValue& other) const {
    return value_ == other.value_;
  }
  bool operator!=(const DecimalValue& other) const {
    return value_ != other.value_;
  }
  bool operator<=(const DecimalValue& other) const {
    return value_ <= other.value_;
  }
  bool operator<(const DecimalValue& other) const {
    return value_ < other.value_;
  }
  bool operator>=(const DecimalValue& other) const {
    return value_ >= other.value_;
  }
  bool operator>(const DecimalValue& other) const {
    return value_ > other.value_;
  }

  DecimalValue operator-() const {
    return DecimalValue(-value_);
  }

  bool is_negative() const { return value_ < 0; }

  /// Compares this and other. Returns 0 if equal, < 0 if this < other and > 0 if
  /// this > other.
  inline int Compare(const DecimalValue& other) const;

  /// Returns a new decimal scaled by from src_type to dst_type.
  /// e.g. If this value was 1100 at scale 3 and the dst_type had scale two, the
  /// result would be 110. (In both cases representing the decimal 1.1).
  inline DecimalValue ScaleTo(int src_scale, int dst_scale, int dst_precision,
      bool* overflow) const;

  /// Implementations of the basic arithmetic operators. In all these functions,
  /// we take the precision and scale of both inputs. The return type is assumed
  /// to be known by the caller (generated by the planner).
  /// Although these functions accept the result scale, that should be seen as
  /// an optimization to avoid having to recompute it in the function. The
  /// functions implement the SQL decimal rules *only* so other result scales are
  /// not valid.
  /// RESULT_T needs to be larger than T to avoid overflow issues.
  template<typename RESULT_T>
  inline DecimalValue<RESULT_T> Add(int this_scale, const DecimalValue& other,
      int other_scale, int result_precision, int result_scale, bool round,
      bool* overflow) const;

  template<typename RESULT_T>
  inline DecimalValue<RESULT_T> Subtract(int this_scale, const DecimalValue& other,
      int other_scale, int result_precision, int result_scale, bool round,
      bool* overflow) const {
    return Add<RESULT_T>(this_scale, -other, other_scale, result_precision,
        result_scale, round, overflow);
  }

  template<typename RESULT_T>
  inline DecimalValue<RESULT_T> Multiply(int this_scale, const DecimalValue& other,
      int other_scale, int result_precision, int result_scale, bool round,
      bool* overflow) const;

  /// is_nan is set to true if 'other' is 0. The value returned is undefined.
  template<typename RESULT_T>
  inline DecimalValue<RESULT_T> Divide(int this_scale, const DecimalValue& other,
      int other_scale, int result_precision, int result_scale, bool round,
      bool* is_nan, bool* overflow) const;

  template<typename RESULT_T>
  inline DecimalValue<RESULT_T> Mod(int this_scale, const DecimalValue& other,
      int other_scale, int result_precision, int result_scale, bool round,
      bool* is_nan, bool* overflow) const;

  /// Compares this and other. Returns 0 if equal, < 0 if this < other and > 0 if
  /// this > other.
  inline int Compare(int this_scale, const DecimalValue& other, int other_scale) const;

  /// Comparison utilities.
  inline bool Eq(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) == 0;
  }
  inline bool Ne(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) != 0;
  }
  inline bool Ge(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) >= 0;
  }
  inline bool Gt(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) > 0;
  }
  inline bool Le(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) <= 0;
  }
  inline bool Lt(int this_scale, const DecimalValue& other, int other_scale) const {
    return Compare(this_scale, other, other_scale) < 0;
  }

  /// Returns the underlying storage. For a particular storage size, there is
  /// only one representation for any decimal and the storage is directly comparable.
  inline const T& value() const { return value_; }
  inline T& value() { return value_; }

  /// Returns the value of the decimal before the decimal point.
  inline const T whole_part(int scale) const;

  /// Returns the value of the decimal after the decimal point.
  inline const T fractional_part(int scale) const;

  /// Returns the value as an integer, setting overflow to true on overflow,
  /// and leaving unchanged otherwise.  Rounds to the nearest integer, defined
  /// as half / round away from zero.  Template parameter RESULT_T should be a
  /// UDF Val type which defines an integer underlying type as underlying_type_t
  template <typename RESULT_T>
  inline typename RESULT_T::underlying_type_t ToInt(int scale, bool* overflow) const;

  /// Returns an approximate double for this decimal.
  inline double ToDouble(int scale) const;

  inline uint32_t Hash(int seed = 0) const;

  std::string ToString(const ColumnType& type) const;
  std::string ToString(int precision, int scale) const;

  inline DecimalValue<T> Abs() const;

 private:
  T value_;

  /// Returns in *x_val and *y_val, the adjusted values so that both are at
  /// max(x_scale, y_scale) scale. The scale is the number of digits after the decimal.
  /// Returns true if the adjustment causes overflow in which case the values in
  /// x_scaled and y_scaled are unmodified.
  template <typename RESULT_T>
  static inline bool AdjustToSameScale(const DecimalValue& x, int x_scale,
      const DecimalValue& y, int y_scale, int result_precision, RESULT_T* x_scaled,
      RESULT_T* y_scaled);
};

typedef DecimalValue<int32_t> Decimal4Value;
typedef DecimalValue<int64_t> Decimal8Value;
/// TODO: should we support Decimal12Value? We pad it to 16 bytes in the tuple
/// anyway.
typedef DecimalValue<int128_t> Decimal16Value;

inline std::ostream& operator<<(std::ostream& os, const Decimal4Value& d) {
  return os << d.value();
}
inline std::ostream& operator<<(std::ostream& os, const Decimal8Value& d) {
  return os << d.value();
}
inline std::ostream& operator<<(std::ostream& os, const Decimal16Value& d) {
  return os << d.value();
}

}

#endif
