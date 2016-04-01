#ifndef BROKER_UTIL_OPERATORS_HH
#define BROKER_UTIL_OPERATORS_HH

namespace broker {
namespace util {

template <class T, class U = T>
struct equality_comparable {
  friend bool operator!=(const T& x, const U& y) {
    return !(x == y);
  }
};

template <class T, class U = T>
struct less_than_comparable {
  friend bool operator>(const T& x, const U& y) {
    return y < x;
  }
  friend bool operator<=(const T& x, const U& y) {
    return !(y < x);
  }
  friend bool operator>=(const T& x, const U& y) {
    return !(x < y);
  }
};

template <class T, class U = T>
struct totally_ordered : equality_comparable<T, U>,
                         less_than_comparable<T, U> {};

} // namespace util
} // namespace broker

#endif // BROKER_UTIL_OPERATORS_HH
