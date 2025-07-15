#include "rest_rpc/rpc_client.hpp"
#include "rest_rpc/rpc_server.h"



// client 发送的报文格式
// 头部 header
// struct rpc_header
//   {
//     uint8_t magic;          // 魔法数字，用来识别RPC协议类型 39
//     request_type req_type;  // 请求类型；分为请求远程调用和订阅发布两种类型
//     uint32_t body_len;      // 请求体长度
//     uint64_t req_id;        // 请求ID；订阅时，req_id为0
//     uint32_t func_id;       // 函数ID，是一个MD5的hash值
//   };
// 正文 body
// 内容均会被序列化
// 订阅情况下，为key + token
// 请求情况下，为参数列表
// 若为空，则为心跳报文


// server 发送的报文格式
// 头部 header
// 同client
// 正文 body
// 内容均会被序列化
// 订阅情况下，为 result_code状态码, key + toekn, data()；该报文为某个client订阅的频道的消息
// 请求情况下，为 result，包含result_code状态码和结果，若出错则结果为错误信息，否则为执行结果