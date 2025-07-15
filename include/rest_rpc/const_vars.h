#pragma once
#include <cstdint>

// 这个文件定义了许多RPC的enum、struct和常量

namespace rest_rpc
{
  // 返回结果
  enum class result_code : std::int16_t
  {
    OK = 0,
    FAIL = 1,
  };

  // 错误码
  enum class error_code
  {
    OK,
    UNKNOWN,
    FAIL,
    TIMEOUT,
    CANCEL,
    BADCONNECTION,
  };

  // 请求类型
  enum class request_type : uint8_t
  {
    req_res,  // 请求响应？
    sub_pub   // 订阅发布？
  };

  // 消息结构体，包含了请求ID、请求类型和一个指向字符串内容的智能指针
  struct message_type
  {
    std::uint64_t req_id;   // 请求ID
    request_type req_type;  // 请求类型
    std::shared_ptr<std::string> content; // 指向字符串内容的智能指针
  };

  static const uint8_t MAGIC_NUM = 39;  // 魔法数字

#pragma pack(4) // 4字节对齐
  struct rpc_header
  {
    uint8_t magic;          // 魔法数字，用来识别RPC协议类型
    request_type req_type;  // 请求类型
    uint32_t body_len;      // 请求体长度
    uint64_t req_id;        // 请求ID；订阅时，req_id为0
    uint32_t func_id;       // 函数ID，应该是一个MD5的hash值
  };
#pragma pack()

  static const size_t MAX_BUF_LEN = 1048576 * 10;
  static const size_t HEAD_LEN = sizeof(rpc_header);
  static const size_t INIT_BUF_SIZE = 2 * 1024;
} // namespace rest_rpc