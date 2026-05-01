/**
 * Copyright ...
 * Utilities.
 */

#ifndef ANN_HELPER_EXPR_H
#define ANN_HELPER_EXPR_H

#include <utility>
#include <type_traits>
#if __cplusplus < 201703L
#include <functional>
#endif /* c++14 or less */

#include <vtl/internal/container.hpp>

#if defined(__cplusplus) && __cplusplus >= 202302L
#define Assume(expr) [[assume(expr)]]
#elif defined(__clang_major__)
#define Assume(expr) __builtin_assume(expr)
#else
/* intentionally make it open (without do while) to hint compilers */
#define Assume(expr) Assert(expr); (expr) ? static_cast<void>(0) : __builtin_unreachable()
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
#define CONSTEXPR_IF if constexpr
#else
#define CONSTEXPR_IF if
#endif

#ifndef _GLIBCXX17_CONSTEXPR
#if __cplusplus >= 201703L
#define _GLIBCXX17_CONSTEXPR constexpr
#else
#define _GLIBCXX17_CONSTEXPR
#endif
#endif

#ifdef _MSC_VER 
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif defined(__cplusplus) && __cplusplus >= 202002L
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS
#endif

#if __cplusplus >= 201703L  /* c++17 or greater */
#define IS_INVOCABLE_R(call_type, ret_type, ...) std::is_invocable_r_v<ret_type, call_type, __VA_ARGS__>
#define IS_INVOCABLE(call_type, ...) std::is_invocable_v<call_type, __VA_ARGS__>
#define RESULT_OF(call_type, ...) std::invoke_result_t<call_type, __VA_ARGS__>
#else
#define IS_INVOCABLE_R(call_type, ret_type, ...) std::is_constructible< \
    std::function<ret_type(__VA_ARGS__)>,                               \
    std::reference_wrapper<typename std::remove_reference<call_type>::type>>::value
#define IS_INVOCABLE(call_type, ...) std::is_constructible< \
    std::function<void(__VA_ARGS__)>,                       \
    std::reference_wrapper<typename std::remove_reference<call_type>::type>>::value
#if __cplusplus >= 201402L    /* c++14 */
#define RESULT_OF(call_type, ...) std::result_of_t<call_type(__VA_ARGS__)>
#else /* c++11 or less */
#define RESULT_OF(call_type, ...) typename std::result_of<call_type(__VA_ARGS__)>::type
#endif
#endif
#define PureType(in_type) typename std::remove_cv<typename std::remove_reference<in_type>::type>::type

namespace ann_helper {
namespace internal {
template <typename>
constexpr std::false_type has_destroyer_h(long);
template <typename T>
constexpr auto has_destroyer_h(int) -> decltype(std::declval<T>().destroy(), std::true_type{});
template <typename T>
using has_destroyer = decltype(has_destroyer_h<T>(0));
template <typename T>
void optional_destroy(T &obj, std::true_type const &) { obj.destroy(); }
template <typename T>
void optional_destroy(T &obj, std::false_type const &) {}
template <typename>
constexpr std::false_type has_constructor_with_alloc_h(long);
template <typename T>
constexpr auto has_constructor_with_alloc_h(int) ->
    decltype(T::construct_with_alloc, std::true_type{});
template <typename T>
using has_constructor_with_alloc = decltype(has_constructor_with_alloc_h<T>(0));
template <typename T>
constexpr bool constructor_with_alloc(std::false_type const &)
{
    return !std::is_scalar<T>::value &&
        !std::is_trivially_default_constructible<T>::value;
}
template <typename T>
constexpr bool constructor_with_alloc(std::true_type const &)
{
    return !std::is_scalar<T>::value &&
        !std::is_trivially_default_constructible<T>::value &&
        T::construct_with_alloc;
}

template <typename T>
struct constructor_with_alloc_st {
    static constexpr bool value = constructor_with_alloc<T>(has_constructor_with_alloc<T>{});
};
template <typename T1, typename T2>
struct constructor_with_alloc_st<std::pair<T1, T2>> {
    static constexpr bool value =
        constructor_with_alloc<T1>(has_constructor_with_alloc<T1>{}) ||
        constructor_with_alloc<T2>(has_constructor_with_alloc<T2>{});
};
} /* internal */
/* call destroy if the object has a destroy() function */
template <typename T>
inline void optional_destroy(T &obj)
    { internal::optional_destroy(obj, internal::has_destroyer<T>{}); }
/* return constructor_with_alloc if a class has one, otherwise true */
template <typename T>
constexpr bool constructor_with_alloc =
    internal::constructor_with_alloc_st<T>::value;
template <typename T>
constexpr bool constructor_need_ctx = constructor_with_alloc<T>;

inline void print_size(size_t nbytes, char *buf)
{
    if (nbytes < 1024) {
        sprintf(buf, "%lu B", nbytes);
    } else if (nbytes < 1024 * 1024) {
        sprintf(buf, "%.2f KB", nbytes / 1024.0);
    } else if (nbytes < 1024 * 1024 * 1024) {
        sprintf(buf, "%.2f MB", nbytes / 1024.0 / 1024.0);
    } else {
        sprintf(buf, "%.2f GB", nbytes / 1024.0 / 1024.0 / 1024.0);
    }
}
} /* namespace ann_helper */

#endif /* ANN_HELPER_EXPR_H */
