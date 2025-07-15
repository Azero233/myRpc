#ifndef REST_RPC_META_UTIL_HPP
#define REST_RPC_META_UTIL_HPP

#include "cplusplus_14.h"
#include <functional>

// 实现一些关于tuple处理的元函数

namespace rest_rpc
{
  // 对tuple中的每个元素执行f函数（可能会改变tuple元素值）
  template <typename... Args, typename Func, std::size_t... Idx>
  void for_each(const std::tuple<Args...> &t, Func &&f,
                nonstd::index_sequence<Idx...>)
  {
    // 老实说，这句我有点疑问；这里使用逗号表达式，因此 (f(std::get<Idx>(t)), void(), 0) == 0，但是为什么需要void()？
    (void)std::initializer_list<int>{(f(std::get<Idx>(t)), void(), 0)...};
  }

  // 对tuple中的每个元素执行f函数（可能会改变tuple元素值），f要求传入两个参数，第一个是tuple元素，第二个是tuple元素在tuple中的位置
  template <typename... Args, typename Func, std::size_t... Idx>
  void for_each_i(const std::tuple<Args...> &t, Func &&f,
                  nonstd::index_sequence<Idx...>)
  {
    // integral_constant<size_t, Idx> 重写了类型转换函数，可以直接被隐式转换为 size_t 类型的 Idx
    (void)std::initializer_list<int>{
        (f(std::get<Idx>(t), std::integral_constant<size_t, Idx>{}), void(),
         0)...};
  }

  template <typename T>
  struct function_traits;

  // 有参函数萃取机
  template <typename Ret, typename Arg, typename... Args>
  struct function_traits<Ret(Arg, Args...)>
  {
  public:
    enum
    {
      arity = sizeof...(Args) + 1 // arity 函数参数个数
    };
    typedef Ret function_type(Arg, Args...);  // function_type 函数类型
    typedef Ret return_type;                  // return_type 返回类型
    using stl_function_type = std::function<function_type>; // stl_function_type std::function<function_type> 类型
    typedef Ret (*pointer)(Arg, Args...);                   // pointer 函数指针类型

    typedef std::tuple<Arg, Args...> tuple_type;      // tuple_type 参数类型(tuple形式)
    typedef std::tuple<
        nonstd::remove_const_t<nonstd::remove_reference_t<Args>>...>
        bare_tuple_type;                              // bare_tuple_type 参数Args的类型(非const非引用形式)
    using args_tuple =
        std::tuple<std::string, Arg,
                   nonstd::remove_const_t<nonstd::remove_reference_t<Args>>...>;  // args_tuple
    using args_tuple_2nd =
        std::tuple<std::string,
                   nonstd::remove_const_t<nonstd::remove_reference_t<Args>>...>;  // args_tuple_2nd
  };

  // 无参函数萃取机
  template <typename Ret>
  struct function_traits<Ret()>
  {
  public:
    enum
    {
      arity = 0
    };
    typedef Ret function_type();
    typedef Ret return_type;
    using stl_function_type = std::function<function_type>;
    typedef Ret (*pointer)();

    typedef std::tuple<> tuple_type;
    typedef std::tuple<> bare_tuple_type;
    using args_tuple = std::tuple<std::string>;
    using args_tuple_2nd = std::tuple<std::string>;
  };

  // 函数指针萃取机，直接使用function_traits<Ret(Args...)>
  template <typename Ret, typename... Args>
  struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)>
  {
  };

  // std::function萃取机，直接使用function_traits<Ret(Args...)>
  template <typename Ret, typename... Args>
  struct function_traits<std::function<Ret(Args...)>>
      : function_traits<Ret(Args...)>
  {
  };

  // 成员函数萃取机，要求仅能有一个可访问的成员函数，直接使用function_traits<Ret(Args...)>
  template <typename ReturnType, typename ClassType, typename... Args>
  struct function_traits<ReturnType (ClassType::*)(Args...)>
      : function_traits<ReturnType(Args...)>
  {
  };

  // const成员函数萃取机，要求仅能有一个可访问的成员函数，直接使用function_traits<Ret(Args...)>
  template <typename ReturnType, typename ClassType, typename... Args>
  struct function_traits<ReturnType (ClassType::*)(Args...) const>
      : function_traits<ReturnType(Args...)>
  {
  };

  // 函数对象萃取机，直接使用function_traits<Ret(Args...)>
  template <typename Callable>
  struct function_traits : function_traits<decltype(&Callable::operator())>
  {
  };

  // 去掉类型T的const和reference，所得到的类型
  template <typename T>
  using remove_const_reference_t =
      nonstd::remove_const_t<nonstd::remove_reference_t<T>>;

  // 使用index_sequence的元素构造tuple
  template <size_t... Is>
  auto make_tuple_from_sequence(nonstd::index_sequence<Is...>)
      -> decltype(std::make_tuple(Is...))
  {
    std::make_tuple(Is...);
  }

  // 构造tuple，元素为0，1，2，...，N-1
  template <size_t N>
  constexpr auto make_tuple_from_sequence()
      -> decltype(make_tuple_from_sequence(nonstd::make_index_sequence<N>{}))
  {
    return make_tuple_from_sequence(nonstd::make_index_sequence<N>{});
  }

  namespace detail
  {
    // 仅在i在index_sequence<Is...>的变参列表中时，将i作为f的唯一参数调用f（f中可能会使用该下标访问tuple中的元素）
    template <class Tuple, class F, std::size_t... Is>
    void tuple_switch(const std::size_t i, Tuple &&t, F &&f,
                      nonstd::index_sequence<Is...>)
    {
      (void)std::initializer_list<int>{
          (i == Is &&
           ((void)std::forward<F>(f)(std::integral_constant<size_t, Is>{}), 0))...};
    }
  } // namespace detail

  // 将i作为f的唯一参数调用f
  template <class Tuple, class F>
  inline void tuple_switch(const std::size_t i, Tuple &&t, F &&f)
  {
    constexpr auto N = std::tuple_size<nonstd::remove_reference_t<Tuple>>::value;

    detail::tuple_switch(i, std::forward<Tuple>(t), std::forward<F>(f),
                         nonstd::make_index_sequence<N>{});
  }

  // nth_type_of 为 std::tuple<Args...>中第N个元素（0开始）的类型
  template <int N, typename... Args>
  using nth_type_of = nonstd::tuple_element_t<N, std::tuple<Args...>>;

  // last_type_of 为 std::tuple<Args...>中最后一个元素的类型
  template <typename... Args>
  using last_type_of = nth_type_of<sizeof...(Args) - 1, Args...>;
} // namespace rest_rpc

#endif // REST_RPC_META_UTIL_HPP
