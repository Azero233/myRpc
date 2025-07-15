// 处理序列化和反序列化

#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include <msgpack.hpp>  // 第三方库，实现序列化和反序列化哦

namespace rest_rpc
{
  namespace rpc_service
  {

    using buffer_type = msgpack::sbuffer; // 序列化的buffer类型

    // 处理序列化和反序列化的结构体
    struct msgpack_codec
    {
      const static size_t init_size = 2 * 1024; // 序列化buffer的默认大小

      // 将多个参数打包，返回打包的buffer
      template <typename... Args>
      static buffer_type pack_args(Args &&...args)
      {
        buffer_type buffer(init_size);
        msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
        return buffer;
      }

      // 将多个参数打包，以string形式返回打包后的内容，第一个参数必须是enum类型（result_code类型，表示执行状态）
      template <typename Arg, typename... Args,
                typename = typename std::enable_if<std::is_enum<Arg>::value>::type> // SFINAE，如果Arg是enum类型，则启用此函数
      static std::string pack_args_str(Arg arg, Args &&...args)
      {
        buffer_type buffer(init_size);
        msgpack::pack(buffer,
                      std::forward_as_tuple((int)arg, std::forward<Args>(args)...));  // 这里将enum类型变量强制转换为int，并序列化
        return std::string(buffer.data(), buffer.size());
      }

      // 打包（序列化）单个对象t，返回打包后的buffer
      template <typename T>
      buffer_type pack(T &&t) const
      {
        buffer_type buffer;
        msgpack::pack(buffer, std::forward<T>(t));
        return buffer;
      }

      // 反序列化data的数据，并以类型T返回
      template <typename T>
      T unpack(char const *data, size_t length)
      {
        try
        {
          msgpack::unpack(msg_, data, length);  // unpack函数需要传入buffer的指针data和长度length，将data中的数据反序列化到msg_中
          return msg_.get().as<T>();
        }
        catch (...)
        {
          throw std::invalid_argument("unpack failed: Args not match!");
        }
      }

    private:
      msgpack::unpacked msg_;
    };
  } // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_CODEC_H_