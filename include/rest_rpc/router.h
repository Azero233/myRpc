#ifndef REST_RPC_ROUTER_H_
#define REST_RPC_ROUTER_H_

#include "codec.h"         // 序列化与反序列化
#include "md5.hpp"         // md5加密，返回uint32_t
#include "meta_util.hpp"   // 一些与tuple、function有关的函数模版
#include "string_view.hpp" // 加载c++17的string_view，否则加载自己实现的版本
#include "use_asio.hpp"    // 加载asio库
#include <functional>
#include <string>
#include <unordered_map>

// 基于连接对象 connection，注册、执行和删除函数

namespace rest_rpc
{
  namespace rpc_service
  {
    class connection; // 连接对象；定义在connection.h中

    // router的错误状态
    enum class router_error
    {
      ok,
      no_such_function,
      has_exception,
      unknown
    };

    // 路由结果，存放一个router_error和string；其中router_error描述函数执行状态，result描述了错误信息，需要msgpack序列化
    struct route_result_t
    {
      router_error ec = router_error::unknown;
      std::string result; // 执行结果/错误描述，使用msgpack序列化和反序列化
    };

    // is_pub 为 false 的情况，显然什么都不做（实际上不指定模版的is_pub情况下，默认使用该版本）
    // 实现对tuple中最后一个元素的反序列化
    template <typename Tuple, bool is_pub>
    class helper_t
    {
    public:
      helper_t(Tuple &tp) : tp_(tp) {}

      void operator()() {}

    private:
      Tuple &tp_;
    };

    // is_pub 为 true 的情况，反序列化最后一个元素
    template <typename Tuple>
    class helper_t<Tuple, true>
    {
    public:
      helper_t(Tuple &tp) : tp_(tp) {}

      // 取出tuple的最后一个对象，并对其反序列化（仍存放在tuple中）
      void operator()()
      {
        auto &arg = std::get<std::tuple_size<Tuple>::value - 1>(tp_);
        msgpack_codec codec;
        arg = codec.unpack<std::string>(arg.data(), arg.size());
      }

    private:
      Tuple &tp_;
    };

    // 该对象管理函数注册、执行和删除
    class router : asio::noncopyable // 继承asio::noncopyable 无法在类外构造，派生类对象不能被复制
    {
    public:
      // 注册函数f；is_pub 默认是false
      template <bool is_pub = false, typename Function>
      // void register_handler(std::string const &name, Function f, bool pub = false)  // bool pub = false 并未使用，因此重写
      void register_handler(std::string const &name, Function f)
      {
        uint32_t key = MD5::MD5Hash32(name.data());           // 使用md5加密name，得到一个uint32_t；注意，即使是不同的name，md5加密后得到的uint32_t也可能相同
        if (key2func_name_.find(key) != key2func_name_.end()) // key 已经存在于 key2func_name_ 中
        {
          // 抛出异常
          throw std::invalid_argument("duplicate registration key !");
        }
        else
        {
          // 加入 key2func_name_ 中
          key2func_name_.emplace(key, name);
          return register_nonmember_func<is_pub>(key, std::move(f));
        }
      }

      // 注册成员函数f，需要传入调用对象；is_pub 默认是false
      template <bool is_pub = false, typename Function, typename Self>
      void register_handler(std::string const &name, const Function &f, Self *self)
      {
        uint32_t key = MD5::MD5Hash32(name.data()); // 使用md5加密name
        if (key2func_name_.find(key) != key2func_name_.end())
        {
          throw std::invalid_argument("duplicate registration key !");
        }
        else
        {
          key2func_name_.emplace(key, name);
          return register_member_func<is_pub>(key, f, self);
        }
      }

      // 使用函数名，删去注册的函数
      void remove_handler(std::string const &name)
      {
        uint32_t key = MD5::MD5Hash32(name.data());
        this->map_invokers_.erase(key);
        key2func_name_.erase(key);
      }

      // 使用函数名哈希值，返回对应的函数名
      std::string get_name_by_key(uint32_t key)
      {
        auto it = key2func_name_.find(key);
        if (it != key2func_name_.end())
        {
          return it->second;
        }
        return std::to_string(key);
      }

      // 传入函数名哈希值，和连接对象指针，执行对应的函数，返回route_result_t对象（函数执行成功，result为result_code::ok和执行结果（函数有返回值），否则result为错误信息）
      template <typename T>
      route_result_t route(uint32_t key, nonstd::string_view data,
                           std::weak_ptr<T> conn)
      {
        route_result_t route_result{};
        std::string result;
        try
        {
          msgpack_codec codec;
          auto it = map_invokers_.find(key); // 在map_invokers_中，使用key寻找对应的函数对象
          if (it == map_invokers_.end())
          {
            // 没有找到对应的函数对象，返回错误状态和信息
            result = codec.pack_args_str(
                result_code::FAIL, "unknown function: " + get_name_by_key(key));
            route_result.ec = router_error::no_such_function;
          }
          else
          {
            // 找到对应的函数对象，调用并返回状态：ok
            // it->second的形参是 (std::weak_ptr<connection> conn, nonstd::string_view str, std::string &result)
            it->second(conn, data, result);
            route_result.ec = router_error::ok;
          }
        }
        catch (const std::exception &ex)
        {
          // 捕获异常，返回错误状态和信息
          msgpack_codec codec;
          result = codec.pack_args_str(
              result_code::FAIL,
              std::string("exception occur when call").append(ex.what()));
          route_result.ec = router_error::has_exception;
        }
        catch (...)
        {
          // 捕获异常，返回错误状态和信息
          msgpack_codec codec;
          result = codec.pack_args_str(
              result_code::FAIL, std::string("unknown exception occur when call ")
                                     .append(get_name_by_key(key)));
          // route_result.ec = router_error::no_such_function; // 源代码，这里使用no_such_function不太贴切
          route_result.ec = router_error::has_exception;
        }

        route_result.result = std::move(result);

        return route_result;
      }

      // 默认构造函数
      router() = default;

    private:
      // 禁用拷贝构造函数和移动构造函数
      router(const router &) = delete;
      router(router &&) = delete;

      // f的参数列表为 weak_ptr<connection>，Args...；可以看一看example中的代码，第一个参数都必须是 weak_ptr<connection>
      // 这里使用了std::result_of推断返回类型
      template <typename F, size_t... I, typename... Args>
      static typename std::result_of<F(std::weak_ptr<connection>, Args...)>::type
      call_helper(const F &f, const nonstd::index_sequence<I...> &,
                  std::tuple<Args...> tup, std::weak_ptr<connection> ptr)
      {
        // 执行f，并返回执行结果；即使f返回类型为void也ok，编译器允许这种情况
        return f(ptr, std::move(std::get<I>(tup))...);
      }

      // 当F(std::weak_ptr<connection>, Args...) 的返回值类型 为void 时启用，result封装状态信息
      // 使用 std::enable_if 与 std::is_void 结合，判断F的返回值类型是否为void，void时才会以该模版编译
      // 然而，需要注意，本身std::enable_if的结果为void，函数执行的结果通过 std::string &result 这个引用来传递（结果会被序列化）
      template <typename F, typename... Args>
      static typename std::enable_if<std::is_void<typename std::result_of<
          F(std::weak_ptr<connection>, Args...)>::type>::value>::type // 其实这个很长的返回值类型为 void ；因为enable_if的type默认为void
      call(const F &f, std::weak_ptr<connection> ptr, std::string &result,
           std::tuple<Args...> tp)
      {
        call_helper(f, nonstd::make_index_sequence<sizeof...(Args)>{},
                    std::move(tp), ptr);
        result = msgpack_codec::pack_args_str(result_code::OK); // result 为序列化后的 result_code::OK
      }

      // 当F(std::weak_ptr<connection>, Args...) 的返回值类型 不为void 时启用，result封装状态信息
      template <typename F, typename... Args>
      static typename std::enable_if<!std::is_void<typename std::result_of<
          F(std::weak_ptr<connection>, Args...)>::type>::value>::type // 其实这个很长的返回值类型为 void
      call(const F &f, std::weak_ptr<connection> ptr, std::string &result,
           std::tuple<Args...> tp)
      {
        auto r = call_helper(f, nonstd::make_index_sequence<sizeof...(Args)>{},
                             std::move(tp), ptr); // 将f的执行结果保存到r中
        // msgpack_codec codec;    // 源代码，这句话好像没用，注释了
        result = msgpack_codec::pack_args_str(result_code::OK, r); // result 为序列化后的 result_code::OK 和 r
      }

      // F的参数列表为 (weak_ptr<connection>，Args...)，且可能weak_ptr 为 nullptr
      template <typename F, typename Self, size_t... Indexes, typename... Args>
      static
          typename std::result_of<F(Self, std::weak_ptr<connection>, Args...)>::type // 推断F的返回值类型，F为成员函数
          call_member_helper(const F &f, Self *self,
                             const nonstd::index_sequence<Indexes...> &,
                             std::tuple<Args...> tup,
                             std::weak_ptr<connection> ptr =
                                 std::shared_ptr<connection>{nullptr})
      {
        // 等于(self->*f)，调用self对象的f函数
        return (*self.*f)(ptr, std::move(std::get<Indexes>(tup))...);
      }

      // f为成员函数且返回void时使用，f第一个参数为:weak_ptr<connection>，result封装状态信息
      template <typename F, typename Self, typename... Args>
      static typename std::enable_if<std::is_void<typename std::result_of<
          F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
      call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
                  std::string &result, std::tuple<Args...> tp)
      {
        call_member_helper(f, self,
                           typename nonstd::make_index_sequence<sizeof...(Args)>{},
                           std::move(tp), ptr);
        result = msgpack_codec::pack_args_str(result_code::OK);
      }

      // f为成员函数且不返回void时使用，f第一个参数为:weak_ptr<connection>，result封装状态信息
      template <typename F, typename Self, typename... Args>
      static typename std::enable_if<!std::is_void<typename std::result_of<
          F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
      call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
                  std::string &result, std::tuple<Args...> tp)
      {
        auto r = call_member_helper(
            f, self, typename nonstd::make_index_sequence<sizeof...(Args)>{},
            std::move(tp), ptr);
        result = msgpack_codec::pack_args_str(result_code::OK, r);
      }

      // 注册一个函数，该函数传入std::weak_ptr<connection> conn, nonstd::string_view str（会被反序列化），std::string &result（执行函数对象f的结果）
      // 传入uint32_t的哈希值和一个函数对象f
      template <bool is_pub, typename Function>
      void register_nonmember_func(uint32_t key, Function f)
      {
        this->map_invokers_[key] = [f](std::weak_ptr<connection> conn,
                                       nonstd::string_view str,
                                       std::string &result)
        {
          // 注意一下，这里的代码都在map_invokers_里，也就是数，可以通过map_invokers_调用该函数，执行一个注册了的函数并将结果存入result中
          using args_tuple = typename function_traits<Function>::bare_tuple_type; // args_tuple 为一个tuple，内部元素类型为 f 的第2个到最后一个参数类型
          msgpack_codec codec;                                                    // 处理序列化和反序列化
          try
          {
            auto tp = codec.unpack<args_tuple>(str.data(), str.size()); // 将str反解码为args_tuple类型，并赋值给tp
            helper_t<args_tuple, is_pub>{tp}();                         // is_pub为false的情况下，反序列化tp的最后一个元素
            call(f, conn, result, std::move(tp));                       // 调用f，返回结果在result中且已经序列化了
          }
          catch (std::invalid_argument &e)
          {
            result = codec.pack_args_str(result_code::FAIL, e.what()); // 序列化错误信息，放入result中
          }
          catch (const std::exception &e)
          {
            result = codec.pack_args_str(result_code::FAIL, e.what()); // 序列化错误信息，放入result中
          }
        };
      }

      // 注册一个函数，该函数传入std::weak_ptr<connection> conn, nonstd::string_view str（会被反序列化），std::string &result（执行函数对象f的结果）
      // 传入uint32_t的哈希值，一个函数对象f，和调用该函数的对象self
      template <bool is_pub, typename Function, typename Self>
      void register_member_func(uint32_t key, const Function &f, Self *self)
      {
        this->map_invokers_[key] = [f, self](std::weak_ptr<connection> conn,
                                             nonstd::string_view str,
                                             std::string &result)
        {
          using args_tuple = typename function_traits<Function>::bare_tuple_type;
          msgpack_codec codec; // 处理序列化和反序列化
          try
          {
            auto tp = codec.unpack<args_tuple>(str.data(), str.size()); // tp为tuple，内部元素类型为f的第2个到最后一个参数类型
            helper_t<args_tuple, is_pub>{tp}();                         // is_pub为false的情况下，反序列化tp的最后一个元素
            call_member(f, self, conn, result, std::move(tp));          // 调用f，返回结果在result中且已经序列化了
          };
          catch (std::invalid_argument &e)
          {
            result = codec.pack_args_str(result_code::FAIL, e.what()); // 序列化错误信息，放入result中
          }
          catch (const std::exception &e)
          {
            result = codec.pack_args_str(result_code::FAIL, e.what()); // 序列化错误信息，放入result中
          }
        }

        std::unordered_map<uint32_t, std::function<void(std::weak_ptr<connection>, nonstd::string_view, std::string &)>>
            map_invokers_;                                        // key -> func；使用uint32_t作为key，value为函数对象
        std::unordered_map<uint32_t, std::string> key2func_name_; // key -> funcName；使用uint32_t作为key，value为函数名
      };
    } // namespace rpc_service
  } // namespace rest_rpc

#endif // REST_RPC_ROUTER_H_
