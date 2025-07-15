#ifndef REST_RPC_CONNECTION_H_
#define REST_RPC_CONNECTION_H_

#include "const_vars.h"
#include "cplusplus_14.h"
#include "nonstd_any.hpp" // c++17或以上，使用std::any；否则，自己实现any
#include "router.h"       // 基于连接对象，注册、执行和删除函数
#include "use_asio.hpp"   // asio库
#include <array>
#include <deque>
#include <iostream>
#include <memory>

using asio::ip::tcp;

// connection 实现对router（rpc函数的执行）的封装，同时可以接受rpc请求、发送rpc响应，完成订阅功能

namespace rest_rpc
{
  namespace rpc_service
  {
    // cinatra框架相关，应该是证书、秘钥路径的配置，不使用
    struct ssl_configure
    {
      std::string cert_file;
      std::string key_file;
    };

    // 得，这个类包揽了整个文件的后半部分
    // connection 连接对象，不可被复制
    class connection : public std::enable_shared_from_this<connection>, // 继承enable_shared_from_this<connection>后，可以在类内部使用shared_from_this()方法获取当前对象的shared_ptr，这要求确保该对象是被shared_ptr管理的
                       private asio::noncopyable
    {
    public:
      // asio::io_service 类是 Asio 库中所有 I/O 操作的中心。它负责调度和执行异步操作，包括网络操作、文件 I/O、定时器等。io_service 对象通常在程序的生命周期内只创建一次，并且可以被多个线程同时使用。

      // 构造函数，需要传入io_service对象，超时时间，路由对象
      connection(asio::io_service &io_service, std::size_t timeout_seconds,
                 router &router)
          : socket_(io_service), body_(INIT_BUF_SIZE), timer_(io_service),
            timeout_seconds_(timeout_seconds), has_closed_(false), router_(router)
      {
      }

      // 转调用close()
      ~connection() { close(); }

      // 启动连接，读取请求头
      void start()
      {
        if (is_ssl() && !has_shake_)
        {
          // cinatra框架相关，不走
          async_handshake();
        }
        else
        {
          read_head();
        }
      }

      tcp::socket &socket() { return socket_; }

      bool has_closed() const { return has_closed_; }
      uint64_t request_id() const { return req_id_; }

      // （异步）发送响应；这个方法不会序列化data；订阅消息时，req_id为0
      // 由于这个方法不会序列化data，因此不应该设置在公开接口中
      void response(uint64_t req_id, std::string data,
                    request_type req_type = request_type::req_res)
      {
        assert(data.size() < MAX_BUF_LEN);
        auto sp_data = std::make_shared<std::string>(std::move(data));
        std::weak_ptr<connection> weak = shared_from_this();

        // asio::post()函数用于将一个可调用对象（如函数、函数对象或lambda表达式）放入io_service对象的任务队列中，以便在io_service对象的线程池中异步执行
        asio::post(socket_.get_executor(), [this, weak, sp_data, req_id, req_type]
                   {
      auto conn = weak.lock();  // 获取一个强引用（std::shared_ptr）指向弱引用所跟踪的对象，失败返回一个空指针；这可以保证在当前作用域内，对象不会被销毁
      if (conn) {
        // 资源有效，加入队列等待发送
        response_interal(req_id, std::move(sp_data), req_type);
      } });
      }

      // （异步）发送响应；这个方法先序列化data，随后发送
      template <typename T>
      void pack_and_response(uint64_t req_id, T data)
      {
        auto result =
            msgpack_codec::pack_args_str(result_code::OK, std::move(data));
        response(req_id, std::move(result));
      }

      void set_conn_id(int64_t id) { conn_id_ = id; }

      int64_t conn_id() const { return conn_id_; }

      // 设置 user_data_
      template <typename T>
      void set_user_data(const T &data) { user_data_ = data; }

      // 获取 user_data_
      template <typename T>
      T get_user_data()
      {
        return nonstd::any_cast<T>(user_data_);
      }

      const std::vector<char> &body() const { return body_; }

      // 返回对端网络地址（需要保证已经连接到了远程服务器）
      std::string remote_address() const
      {
        if (has_closed_)
        {
          return "";
        }

        asio::error_code ec;
        auto endpoint = socket_.remote_endpoint(ec);
        if (ec)
        {
          return "";
        }
        return endpoint.address().to_string();
      }

      // 发布消息（先序列化）
      void publish(const std::string &key, const std::string &data)
      {
        auto result = msgpack_codec::pack_args_str(result_code::OK, key, data);
        response(0, std::move(result), request_type::sub_pub);
      }

      // 设置消息订阅的函数
      void set_callback(
          std::function<void(std::string, std::string, std::weak_ptr<connection>)>
              callback)
      {
        callback_ = std::move(callback);
      }

      // delay为true时，下一个rpc响应将会延迟发送
      void set_delay(bool delay) { delay_ = delay; }

      // 设置出错时的回调函数，在rpc_server中设置为 on_net_err_callback_
      void on_network_error(std::function<void(std::shared_ptr<connection>,
                                               std::string)> &on_net_err)
      {
        on_net_err_ = &on_net_err;
      }

      // cinatra框架相关，不使用
      void init_ssl_context(const ssl_configure &ssl_conf)
      {
#ifdef CINATRA_ENABLE_SSL
        unsigned long ssl_options = asio::ssl::context::default_workarounds |
                                    asio::ssl::context::no_sslv2 |
                                    asio::ssl::context::single_dh_use;
        try
        {
          asio::ssl::context ssl_context(asio::ssl::context::sslv23);
          ssl_context.set_options(ssl_options);
          ssl_context.set_password_callback(
              [](std::size_t size,
                 asio::ssl::context_base::password_purpose purpose)
              {
                return "123456";
              });

          asio::error_code ec;
          if (rpcfs::exists(ssl_conf.cert_file, ec))
          {
            ssl_context.use_certificate_chain_file(ssl_conf.cert_file);
          }

          if (rpcfs::exists(ssl_conf.key_file, ec))
            ssl_context.use_private_key_file(ssl_conf.key_file,
                                             asio::ssl::context::pem);

          // ssl_context_callback(ssl_context);
          ssl_stream_ =
              std::make_unique<asio::ssl::stream<asio::ip::tcp::socket &>>(
                  socket_, ssl_context);
        }
        catch (const std::exception &e)
        {
          print(e);
        }
#endif
      }
      //
      // 以下的方法为private，不对外暴露
      //
    private:
      // asio::async_read异步读取rpc头部，存入head_缓存区
      void read_head()
      {
        // reset_timer()在这里的作用是，若 timeout_seconds_ 的时间内没有收到下一个rpc请求，则关闭连接
        reset_timer();                       // 重置定时器时间
        auto self(this->shared_from_this()); // self 为指向当前对象的shared_ptr

        // 好的，下面都是这个函数了：调用asio::async_read，当操作完成或者出错时，调用传入的这个lambda函数
        async_read_head([this, self](asio::error_code ec, std::size_t length)
                        {
      if (!socket_.is_open()) {
        // socket_已经关闭了
        if (on_net_err_) {
          (*on_net_err_)(self, "socket already closed");
        }
        return;
      }

      if (!ec) {
        // 正常情况
        // 1、检查MAGIC_NUM是否为39，如果不是，则报错并关闭连接
        // 2、检查body_len是否大于0且小于MAX_BUF_LEN，是则异步读取rpc请求的body部分，结束流程
        // 3、如果body_len等于0，则表示没有body，可能是心跳包，直接调用read_head
        // 4、否则表明body_len大于MAX_BUF_LEN，报错并关闭连接

        // const uint32_t body_len = *((int*)(head_));
        // req_id_ = *((std::uint64_t*)(head_ + sizeof(int32_t)));
        rpc_header *header = (rpc_header *)(head_); // head_为rpc头部的缓存区
        if (header->magic != MAGIC_NUM) {
          // rpc头部必须以MAGIC_NUM(39)开头，否则报错并关闭连接
          // 这里没有使用on_net_err_处理，可能是认为是协议错误，可能是恶意攻击或程序错误等，不处理，直接关闭连接
          print("protocol error");
          close();
          return;
        }

        req_id_ = header->req_id; // req_id_为rpc请求的id
        const uint32_t body_len = header->body_len; // body_len为rpc请求的body长度
        req_type_ = header->req_type; // req_type_为rpc请求的类型
        if (body_len > 0 && body_len < MAX_BUF_LEN) {
          // 一切正常,调用 read_body
          if (body_.size() < body_len) {
            // 保证缓冲区大小
            body_.resize(body_len);
          }
          // 异步读取rpc请求的body部分
          read_body(header->func_id, body_len);
          return;
        }

        if (body_len == 0) { // nobody, just head, maybe as heartbeat.
          cancel_timer();
          read_head();
        } else {
          // body_len >= MAX_BUF_LEN，报错并关闭连接
          print("invalid body len");
          if (on_net_err_) {
            (*on_net_err_)(self, "invalid body len");
          }
          close();
        }
      } else {
        // 发生错误，打印错误信息、调用on_net_err_，最后关闭连接
        print(ec);
        if (on_net_err_) {
          (*on_net_err_)(self, ec.message());
        }
        close();
      } });
      } // end of read_head

      // asio::async_read读取rpc请求的body，存入成员变量body_中，并根据rpc请求类型调用不同的处理函数（比如request_type::req_res，则会发送rpc响应）
      // 传入函数id和rpc的body大小
      void read_body(uint32_t func_id, std::size_t size)
      {
        auto self(this->shared_from_this()); // self 为指向当前对象的shared_ptr

        // 好的，后面都是这个函数了
        async_read(size, [this, func_id, self](asio::error_code ec,
                                               std::size_t length)
                   {
          cancel_timer(); // 取消定时器

          if (!socket_.is_open()) {
            // socket_已经关闭了
            if (on_net_err_) {
              (*on_net_err_)(self, "socket already closed");
            }
            return;
          }

          if (!ec) {
            // 正常情况
            read_head();  // 这里read_head设置了定时器，异步接收下一个rpc头部；并不是现在立马处理下一个rpc头部，因为正在处理当前的rpc请求body
            if (req_type_ == request_type::req_res) {
              // 请求响应
              // 执行指定函数，返回值ret内封装了状态码router_error和执行结果string
              route_result_t ret = router_.route<connection>(
                  func_id, nonstd::string_view{body_.data(), length},
                  this->shared_from_this());  // 你TM这里到底为什么要传一个弱引用啊？
              if (delay_) {
                // 延迟响应（尚不知道是什么机制）
                delay_ = false;
              } else {
                // 立即响应，发送消息；这里仅发送了函数返回值result，没有发送状态码
                response_interal(
                    req_id_, std::make_shared<std::string>(std::move(ret.result)));
              }
            } else if (req_type_ == request_type::sub_pub) {
              // 订阅发布
              try {
                // 加入订阅列表
                msgpack_codec codec;
                // p为tuple<string, string>；length为asio::async_read读取的数据长度；这里将读取的rpc的body反序列化了
                auto p = codec.unpack<std::tuple<std::string, std::string>>(
                    body_.data(), length);
                // 将当前连接对象加入订阅列表，get<0>(p)为key， get<1>(p)为token
                callback_(std::move(std::get<0>(p)), std::move(std::get<1>(p)),
                          this->shared_from_this());
              } catch (const std::exception &ex) {
                print(ex);
                if (on_net_err_) {
                  (*on_net_err_)(self, ex.what());
                }
              }
            }
          } else {
            // 发生错误，调用on_net_err_
            if (on_net_err_) {
              (*on_net_err_)(self, ec.message());
            }
          } });
      } // end of read_body

      // 将rpc响应写入队列，并检查队列中是否只有一个消息，如果是则立即发送；发送的rpc响应报文的body由data构成
      void response_interal(uint64_t req_id, std::shared_ptr<std::string> data,
                            request_type req_type = request_type::req_res)
      {
        assert(data->size() < MAX_BUF_LEN); // 检查数据大小

        write_queue_.emplace_back(message_type{req_id, req_type, std::move(data)});
        if (write_queue_.size() > 1)
        {
          // 有多个消息在队列中，不需要立即发送，因为前一个异步写任务会自动调用write()发送下一个消息
          return;
        }
        // 只有一个消息在队列中，立即发送
        write();
      }

      // asio::async_read写入回应报文，回调函数为 on_write
      void write()
      {
        auto &msg = write_queue_.front();                             // 获取队列中的第一个消息
        write_size_ = (uint32_t)msg.content->size();                  // rpc执行结果/错误信息 的大小
        header_ = {MAGIC_NUM, msg.req_type, write_size_, msg.req_id}; // 构造rpc头部
        std::array<asio::const_buffer, 2> write_buffers;              // 分别存放rpc头部和rpc内容；asio::async_write方法可以传入这样的buffer
        write_buffers[0] = asio::buffer(&header_, sizeof(rpc_header));
        write_buffers[1] = asio::buffer(msg.content->data(), write_size_); // 这里仅封装msg.content->data()的内容，并没有从队列中弹出；在异步写结束后，on_write()会从队列中弹出

        auto self = this->shared_from_this();
        async_write(write_buffers,
                    [this, self](asio::error_code ec, std::size_t length)
                    {
                      on_write(ec, length);
                    });
      }

      // asio::async_write的回调函数（为什么async_read不单独写一个回调函数）
      // 若队列中还有消息，继续调用write()发送
      void on_write(asio::error_code ec, std::size_t length)
      {
        if (ec)
        {
          // 发生错误，打印信息并调用on_net_err_
          print(ec);
          if (on_net_err_)
          {
            (*on_net_err_)(shared_from_this(), ec.message());
          }
          close(false);
          return;
        }

        if (has_closed())
        {
          return;
        }

        write_queue_.pop_front();

        if (!write_queue_.empty())
        {
          // 队列中还有消息，调用asio::async_write继续发送
          write();
        }
      }

      // cinatra框架相关，不使用
      void async_handshake()
      {
#ifdef CINATRA_ENABLE_SSL
        auto self = this->shared_from_this();
        ssl_stream_->async_handshake(asio::ssl::stream_base::server,
                                     [this, self](const asio::error_code &error)
                                     {
                                       if (error)
                                       {
                                         print(error);
                                         if (on_net_err_)
                                         {
                                           (*on_net_err_)(self, error.message());
                                         }
                                         close();
                                         return;
                                       }

                                       has_shake_ = true;
                                       read_head();
                                     });
#endif
      }

      // cinatra框架相关，返回false
      bool is_ssl() const
      {
#ifdef CINATRA_ENABLE_SSL
        return ssl_stream_ != nullptr;
#else
        return false;
#endif
      }

      // 调用asio::async_read，读取rpc头部，当操作完成或者出错时，调用handler
      template <typename Handler>
      void async_read_head(Handler handler)
      {
        if (is_ssl())
        {
          // 不走
#ifdef CINATRA_ENABLE_SSL
          asio::async_read(*ssl_stream_, asio::buffer(head_, HEAD_LEN),
                           std::move(handler));
#endif
        }
        else
        {
          // asio::buffer需要指定缓冲区的大小，head_指向缓冲区的起始位置
          asio::async_read(socket_, asio::buffer(head_, HEAD_LEN),
                           std::move(handler));
        }
      }

      // 调用asio::async_read，读取rpc的body，当操作完成或者出错时，调用handler；size_to_read为body的大小
      template <typename Handler>
      void async_read(size_t size_to_read, Handler handler)
      {
        if (is_ssl())
        {
#ifdef CINATRA_ENABLE_SSL
          asio::async_read(*ssl_stream_, asio::buffer(body_.data(), size_to_read),
                           std::move(handler));
#endif
        }
        else
        {
          // asio::buffer需要指定缓冲区的大小，body_.data()指向缓冲区的起始位置
          asio::async_read(socket_, asio::buffer(body_.data(), size_to_read),
                           std::move(handler));
        }
      }

      // 调用asio::async_write，写入rpc响应，当操作完成或者出错时，调用handler
      // buffers为asio::buffer，包含rpc响应的头部和内容
      template <typename BufferType, typename Handler>
      void async_write(const BufferType &buffers, Handler handler)
      {
        if (is_ssl())
        {
#ifdef CINATRA_ENABLE_SSL
          asio::async_write(*ssl_stream_, buffers, std::move(handler));
#endif
        }
        else
        {
          // asio::async_write是异步写操作，将buffers中的数据写入socket_，当操作完成或者出错时，调用handler
          asio::async_write(socket_, buffers, std::move(handler));
        }
      }

      // 重置定时器；当网络连接在timeout_seconds_内没有活动时，自动关闭连接
      void reset_timer()
      {
        if (timeout_seconds_ == 0)
        {
          return;
        }

        auto self(this->shared_from_this());                             // 指向自己的shared_ptr；异步操作环境下，安全地管理对象的生命周期
        timer_.expires_from_now(std::chrono::seconds(timeout_seconds_)); // 定时器在 timeout_seconds_ 秒后过期

        // async_wait 启动一个异步等待操作，当定时器到期时，调用传入的handler；
        // async_wait 方法需要与 asio::io_context 配合使用，以实现定时器到期时异步调用handler
        timer_.async_wait([this, self](const asio::error_code &ec)
                          {
          if (has_closed()) {
            // 已经关闭连接了，直接返回
            return;
          }
          if (ec) {
            // 出错，直接返回；当我们调用 cancel_timer() 时，传入的ec为asio::error::operation_aborted，也就是说，定时器被取消
            return;
          }
          // LOG(INFO) << "rpc connection timeout";
          // 若不是通过cancel_timer()导致的，而是定时器正常结束计时导致的，则会调用close()方法关闭连接
          close(false); });
      }

      // 调用 timer_.cancel()，取消定时器（保持连接）
      void cancel_timer()
      {
        if (timeout_seconds_ == 0)
        {
          return;
        }
        // cancel 方法用于取消所有等待定时器的异步操作。这意味着，如果有任何异步等待
        // （例如，使用 async_wait）正在等待定时器到期，这些等待将被强制完成。
        // 对于每个被取消的操作，其关联的处理程序（handler）将被调用，并传递一个错误码 asio::error::operation_aborted。
        // 这个错误码表示操作因为定时器被取消而中止。
        // 取消定时器不会改变定时器的到期时间。
        // 即使定时器被取消，它的到期时间仍然是之前设置的时间。
        timer_.cancel();
      }

      // 关闭连接
      void close(bool close_ssl = true)
      {
#ifdef CINATRA_ENABLE_SSL
        if (close_ssl && ssl_stream_)
        {
          asio::error_code ec;
          ssl_stream_->shutdown(ec);
          ssl_stream_ = nullptr;
        }
#endif
        if (has_closed_)
        {
          return;
        }

        asio::error_code ignored_ec;                              // 忽略错误码，即不关心操作是否成功。
        socket_.shutdown(tcp::socket::shutdown_both, ignored_ec); // 关闭套接字的双向数据传输
        socket_.close(ignored_ec);                                // 关闭套接字连接
        has_closed_ = true;
        has_shake_ = false;
      }

      // 打印传入的参数，空格分隔
      template <typename... Args>
      void print(Args... args)
      {
#ifdef _DEBUG
        // _DEBUG 这个宏是在 C/C++ 扩展 的 IntelliSense 配置 中预定义的，可以取消
        std::initializer_list<int>{(std::cout << args << ' ', 0)...};
        std::cout << "\n";
#endif
      }

      // 打印 asio::error_code 的参数和信息
      void print(const asio::error_code &ec) { print(ec.value(), ec.message()); }

      // 打印 std::exception 异常的信息
      void print(const std::exception &ex) { print(ex.what()); }

      tcp::socket socket_; // boost::asio::ip::tcp::socket 类型的对象代表一个 TCP 套接字。这个套接字可以用于建立 TCP 连接、发送和接收数据。需要使用 asio::io_service 类型的对象来创建
#ifdef CINATRA_ENABLE_SSL
      std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket &>> ssl_stream_ =
          nullptr;
#endif
      bool has_shake_ = false; // 是否已经握手，不使用ssl的情况下可以忽略
      char head_[HEAD_LEN];    // 调用async_read时，缓存rpc头部
      std::vector<char> body_; // rpc请求的body缓存区
      std::uint64_t req_id_;   // rpc请求的id
      request_type req_type_;  // rpc请求的类型

      rpc_header header_; // rpc请求的head缓存区；发送请求时会拿来填充数据，作为rpc响应的头部

      uint32_t write_size_ = 0; // rpc执行结果/错误信息 的大小，或者是rpc响应body的大小

      asio::steady_timer timer_;    // boost::asio::steady_timer 类型的对象代表一个定时器，可以用于在指定的时间间隔后执行某个操作。需要使用 asio::io_service(其实是io_context) 类型的对象来创建
      std::size_t timeout_seconds_; // 大概是定时器的时间
      int64_t conn_id_ = 0;         // 连接id；在rpc_server中，可通过id找到该连接对象
      bool has_closed_;             // 连接是否关闭

      std::deque<message_type> write_queue_; // 写队列，存储要发送的消息
      std::function<void(std::string, std::string, std::weak_ptr<connection>)>
          callback_; // 处理订阅的函数；在rpc_server中，在sub_map_中加入订阅者，token_list_中加入token
      std::function<void(std::shared_ptr<connection>, std::string)> *on_net_err_ =
          nullptr;            // 连接网络错误时的回调函数，参数为连接对象和错误信息
      router &router_;        // 路由对象，基于连接对象，注册、执行和删除函数；该路由对象存放在rpc_server中，多个连接共享
      nonstd::any user_data_; // 用户数据，用于存储用户自定义的数据；使用any，可以存储任意类型的数据
      bool delay_ = false;
    };
  } // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_CONNECTION_H_
