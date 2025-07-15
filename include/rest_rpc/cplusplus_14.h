#ifndef REST_RPC_CPLUSPLUS_14_H_
#define REST_RPC_CPLUSPLUS_14_H_

#include <memory>
#include <tuple>
#include <type_traits>

// 这个文件实现了一些c++14中的特性
namespace nonstd
{
  // 对于不同的对象类型，unique_if中定义不同的智能指针类型
  template <class T>
  struct unique_if
  {
    typedef std::unique_ptr<T> single_object; // 指向单对象的智能指针
  };

  template <class T>
  struct unique_if<T[]>
  {
    typedef std::unique_ptr<T[]> unknown_bound; // 指向未知边界的数组（或者，指针）的智能指针
  };

  template <class T, size_t N>
  struct unique_if<T[N]>
  {
    typedef void known_bound; // 指向已知边界的数组，不提供智能指针
  };

  // make_unique函数不支持返回指向定长数组的unique_ptr
  // 对于传入的对象类型T，返回指向其对象/数组的unique_ptr

  // 返回指向单个对象的unique_ptr，支持为该对象的构造函数传入变长参数
  template <class T, class... Args>
  typename unique_if<T>::single_object make_unique(Args &&...args)
  {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
  }

  // 构造指向长度为n的数组的unique_ptr，T可以是对象也可以是不定长数组（指针）
  template <class T>
  typename unique_if<T>::unknown_bound make_unique(size_t n)
  {
    // 若T不为数组，则U 就是 T；若 T 为一维数组，U 就是 T 的元素类型；若 T 为多维数组，U 就是 T 移除第一维的数组类型
    typedef typename std::remove_extent<T>::type U;
    return std::unique_ptr<T>(new U[n]());
  }

  // 不允许使用固定大小的数组创建unique_ptr
  template <class T, class... Args>
  typename unique_if<T>::known_bound make_unique(Args &&...) = delete;

  // 获取可变参数的数量
  template <size_t... Ints>
  struct index_sequence
  {
    using type = index_sequence;
    using value_type = size_t;
    // sizeof... 可在编译时获取参数包的参数数量，即 size() 返回可变参数 Ints 的参数数量
    static constexpr std::size_t size() noexcept { return sizeof...(Ints); }
  };

  // --------------------------------------------------------------

  // 若不是传入两个 index_sequence，则结构体为空
  template <class Sequence1, class Sequence2>
  struct _merge_and_renumber;

  // 将两个 index_sequence 合并成一个，第二个 index_sequence 的每个元素都加上第一个 index_sequence 的元素数量（为什么要这样做？？？）
  template <size_t... I1, size_t... I2>
  struct _merge_and_renumber<index_sequence<I1...>, index_sequence<I2...>>
      : index_sequence<I1..., (sizeof...(I1) + I2)...>
  {
  };

  // --------------------------------------------------------------

  // <>内放入一个数字，生成一个 index_sequence<0, 1, ..., N - 1>
  template <size_t N>
  struct make_index_sequence
      : _merge_and_renumber<typename make_index_sequence<N / 2>::type,
                            typename make_index_sequence<N - N / 2>::type>
  {
  };

  // 递归终止条件，生成一个空的 index_sequence
  template <>
  struct make_index_sequence<0> : index_sequence<>
  {
  };
  // 递归终止条件，生成一个包含单个元素 0 的 index_sequence
  template <>
  struct make_index_sequence<1> : index_sequence<0>
  {
  };

  // 根据传入的对象类型的数量，生成 index_sequence（如<int, int, double>，生成index_sequence<0, 1, 2>）
  template <typename... T>
  using index_sequence_for = make_index_sequence<sizeof...(T)>;

  // 仅在bool值为true时，定义类型 enable_if_t<B, T> 为 T，否则 enable_if_t<B, T> 不能被定义
  template <bool B, class T = void>
  using enable_if_t = typename std::enable_if<B, T>::type;

  // 若 T 为const类型，remove_const_t<T> 为 不带cosnt的T类型，否则 remove_const_t<T> 为 T 类型
  template <typename T>
  using remove_const_t = typename std::remove_const<T>::type;

  // 若 T 为引用（无论左右值引用）类型，remove_reference_t<T> 为 不带引用的T类型，否则 remove_reference_t<T> 为 T 类型
  template <typename T>
  using remove_reference_t = typename std::remove_reference<T>::type;

  // tuple_element_t<I, T> 为tuple T 中第 I 个元素（从0开始）的类型
  template <int I, typename T>
  using tuple_element_t = typename std::tuple_element<I, T>::type;

  // 转换类型 T 为其decayed（衰变）类型
  template <typename T>
  using decay_t = typename std::decay<T>::type;

  // 将Tuple的对应成员作为函数f的传入参数，并返回结果，这些成员为 index_sequence<Idx...> 中的值在 Tuple 中对应的下标的元素
  template <typename F, typename Tuple, size_t... Idx>
  auto apply_helper(F &&f, Tuple &&tp, nonstd::index_sequence<Idx...>)
      -> decltype(std::forward<F>(f)(std::get<Idx>(std::forward<Tuple>(tp))...))
  {
    return std::forward<F>(f)(std::get<Idx>(std::forward<Tuple>(tp))...);
  }

  // 将Tuple中的所有成员作为F中对应位置的参数，并返回结果
  template <typename F, typename Tuple>
  auto apply(F &&f, Tuple &&tp) -> decltype(apply_helper(
                                    std::forward<F>(f), std::forward<Tuple>(tp),
                                    make_index_sequence<std::tuple_size<decay_t<Tuple>>::value>{}))
  {
    return apply_helper(
        std::forward<F>(f), std::forward<Tuple>(tp),
        make_index_sequence<std::tuple_size<decay_t<Tuple>>::value>{});
  }

  // 将后面的变参作为F的参数，并返回结果
  template <typename F, typename... Args>
  auto invoke(F &&f, Args &&...args)
      -> decltype(std::forward<F>(f)(std::forward<Args>(args)...))
  {
    return std::forward<F>(f)(std::forward<Args>(args)...);
  }

} // namespace nonstd

#endif // REST_RPC_CPLUSPLUS_14_H_
