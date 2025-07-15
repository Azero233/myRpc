#pragma once
#include <tuple>
#if __cplusplus > 201402L
#include <string_view>
using string_view = std::string_view;
#else
#include "string_view.hpp"
using namespace nonstd;
#endif

#include "codec.h"

// 将string_view类型的result反序列化，并以传入的类型T返回（会去掉result的状态码）；隐含了 整个result仅包含状态码和T类型的内容这两个部分 这件事

namespace rest_rpc
{
  // 检查result是否有error，有则返回true
  // result为空返回true，否则将其反序列化为tuple<int>，返回第一个元素 != 0 的值
  inline bool has_error(string_view result)
  {
    if (result.empty())
    {
      return true;
    }

    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack<std::tuple<int>>(result.data(), result.size());

    return std::get<0>(tp) != 0;
  }

  // 将result反序列化为tuple<int, T>，返回第二个元素
  // 用于rpc远程调用（而不是订阅）
  // server发送的消息（client收到的消息）的body部分仅有两个内容，状态码和rpc结果
  template <typename T>
  inline T get_result(string_view result)
  {
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack<std::tuple<int, T>>(result.data(), result.size());
    return std::get<1>(tp);
  }

  // 将result反序列化为tuple<int, string>，返回第二个元素
  // 用于rpc远程调用（而不是订阅）
  // server发送的消息（client收到的消息）的body部分仅有两个内容，状态码和rpc结果
  inline std::string get_error_msg(string_view result)
  {
    rpc_service::msgpack_codec codec;
    auto tp =
        codec.unpack<std::tuple<int, std::string>>(result.data(), result.size());
    return std::get<1>(tp);
  }

  // 检查result是否有error，有则抛出logic_error，否则将result反序列化，以T类型返回（去掉状态码）
  template <typename T>
  inline T as(string_view result)
  {
    if (has_error(result))
    {
      throw std::logic_error(get_error_msg(result));
    }

    return get_result<T>(result);
  }
} // namespace rest_rpc