#pragma once

#include <cstddef>

namespace pacer {

/// Provides linear operations on Concrete type.
/// Intended to be used as following:
/// @code{c++}
///     class MyPoint : LinearOperators<MyPoint, double, 2> {
///     public:
///       double &operator[](size_t index) ...
///       double operator[](size_t index) const ...
///     };
///
///     MyPoint a, b;
///     MyPoint c = 3 * a + b / 2;
/// @endcode{c++}
///
template <typename Concrete, typename T, size_t N> struct LinearOperators {
  bool operator==(const Concrete &rhs) const {
    for (size_t i = 0; i < N; ++i)
      if (static_cast<Concrete &>(*this)[i] != rhs[i])
        return false;
    return true;
  }

  bool operator!=(const Concrete &rhs) const {
    for (size_t i = 0; i < N; ++i)
      if (static_cast<Concrete &>(*this)[i] == rhs[i])
        return false;
    return true;
  }

  Concrete &operator+=(const Concrete &other) {
    for (size_t i = 0; i < N; ++i) {
      static_cast<Concrete &>(*this)[i] += other[i];
    }
    return static_cast<Concrete &>(*this);
  }

  Concrete &operator-=(const Concrete &other) {
    for (size_t i = 0; i < N; ++i) {
      static_cast<Concrete &>(*this)[i] -= other[i];
    }
    return static_cast<Concrete &>(*this);
  }

  friend Concrete operator+(Concrete lhs, const Concrete &rhs) {
    return lhs += rhs;
  }
  friend Concrete operator-(Concrete lhs, const Concrete &rhs) {
    return lhs -= rhs;
  }
  Concrete &operator*=(T scalar) {
    for (size_t i = 0; i < N; ++i)
      static_cast<Concrete &>(*this)[i] *= scalar;
    return static_cast<Concrete &>(*this);
  }
  Concrete &operator/=(T scalar) {
    for (size_t i = 0; i < N; ++i)
      static_cast<Concrete &>(*this)[i] /= scalar;
    return static_cast<Concrete &>(*this);
  }

  friend Concrete operator*(Concrete lhs, T scalar) {
    return lhs.LinearOperators::operator*=(scalar);
  }
  friend Concrete operator*(T scalar, Concrete rhs) { return rhs *= scalar; }
  friend Concrete operator/(Concrete lhs, T scalar) {
    return lhs.LinearOperators::operator/=(scalar);
  }

  T Scalar(const Concrete &rhs) const {
    T result = static_cast<const Concrete &>(*this)[0] * rhs[0];
    for (size_t i = 1; i < N; ++i) {
      result += static_cast<const Concrete &>(*this)[i] * rhs[i];
    }
    return result;
  }

  friend T Scalar(const Concrete &lhs, const Concrete &rhs) {
    return lhs.Scalar(rhs);
  }

  T Norm() const {
    T result = static_cast<const Concrete &>(*this)[0] *
               static_cast<const Concrete &>(*this)[0];
    for (size_t i = 1; i < N; ++i) {
      result += static_cast<const Concrete &>(*this)[i] *
                static_cast<const Concrete &>(*this)[i];
    }
    return result;
  }
};

/// Provides pointwise operator
template <typename Concrete, typename T, size_t N> struct PointwiseOperators {
  Concrete &operator*=(const Concrete &rhs) {
    for (size_t i = 0; i < N; ++i)
      static_cast<Concrete &>(*this)[i] *= rhs[i];
    return static_cast<Concrete &>(*this);
  }

  Concrete &operator/=(const Concrete &other) {
    for (size_t i = 0; i < N; ++i)
      static_cast<Concrete &>(*this)[i] /= other[i];
    return static_cast<Concrete &>(*this);
  }

  friend Concrete operator/(Concrete lhs, const Concrete &rhs) {
    return lhs.PointwiseOperators::operator/=(rhs);
  }

  friend Concrete operator*(Concrete lhs, const Concrete &rhs) {
    return lhs.PointwiseOperators::operator*=(rhs);
  }
};

template <typename Concrete, typename T, size_t N>
struct VectorOperators : LinearOperators<Concrete, T, N>,
                         PointwiseOperators<Concrete, T, N> {
  Concrete &operator*=(T scalar) {
    return static_cast<Concrete *>(this)->LinearOperators::operator*=(scalar);
  }
  Concrete &operator*=(const Concrete &other) {
    return static_cast<Concrete *>(this)->PointwiseOperators::operator*=(other);
  }
  Concrete &operator/=(T scalar) {
    return static_cast<Concrete *>(this)->LinearOperators::operator/=(scalar);
  }
  Concrete &operator/=(const Concrete &other) {
    return static_cast<Concrete *>(this)->PointwiseOperators::operator/=(other);
  }
};

} // namespace pacer
