#pragma once
#include "client_util.hpp"
#include "const_vars.h"
#include "md5.hpp"
#include "meta_util.hpp"
#include "use_asio.hpp"
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace rest_rpc
{

  /**
   * The type to indicate the language of the client.
   */
  enum class client_language_t
  {
    CPP = 0,
    JAVA = 1,
  };

  // rpc远程调用的请求结果解析；如果请求成功，则方法 as() 返回请求结果，否则抛出异常
  class req_result
  {
  public:
    req_result() = default;
    req_result(string_view data) : data_(data.data(), data.length()) {}
    // 检查rpc返回消息是否有错误，没有错误返回true
    bool success() const { return !has_error(data_); }

    // 检查 data_ 是否为错误消息，是则抛出异常，否则将 data_ 反序列化，以 T 为类型返回
    template <typename T>
    T as()
    {
      if (has_error(data_))
      {
        std::string err_msg = data_.empty() ? data_ : get_error_msg(data_);
        throw std::logic_error(err_msg);
      }

      return get_result<T>(data_);
    }

    // 检查 data_ 是否为错误消息，是则抛出异常
    void as()
    {
      if (has_error(data_))
      {
        std::string err_msg = data_.empty() ? data_ : get_error_msg(data_);
        throw std::logic_error(err_msg);
      }
    }

  private:
    std::string data_; // 暂存rpc报文的内容

    // 这个类最好只使用一次as方法，否则会多次反序列化，浪费资源；解决方法为内部封存一个反序列化结果
  };

  // 用于使用promise和future处理异步调用结果的结构体，内部封装了future对象来获取rpc调用结果
  template <typename T>
  struct future_result
  {
    uint64_t id;
    std::future<T> future;

    // 调用 future 成员的 wait_for 方法，传递 rel_time 参数。这个方法会阻塞当前 线程，直到异步操作完成或超时
    // 返回 std::future_status 枚举值，表示等待的状态。可能的值有 std::future_status::ready（异步操作已完成）、std::future_status::timeout（等待超时）和 std::future_status::deferred（异步操作尚未开始）。
    template <class Rep, class Per>
    std::future_status wait_for(const std::chrono::duration<Rep, Per> &rel_time)
    {
      return future.wait_for(rel_time);
    }

    // 调用 future 成员的 get 方法，获得异步操作的结果
    T get() { return future.get(); }
  };

  // 以promise/future的方式还是以callback的方式处理rpc调用结果
  enum class CallModel
  {
    future,
    callback
  };
  const constexpr auto FUTURE = CallModel::future; // 常量；client内部部分函数固定使用future方式处理rpc调用结果

  const constexpr size_t DEFAULT_TIMEOUT = 5000; // milliseconds

  class rpc_client : private asio::noncopyable
  {
  public:
    rpc_client()
        : socket_(ios_), work_(std::make_shared<asio::io_context::work>(ios_)),
          deadline_(ios_), body_(INIT_BUF_SIZE)
    {
      thd_ = std::make_shared<std::thread>([this]
                                           { ios_.run(); });
    }

    rpc_client(client_language_t client_language,
               std::function<void(long, const std::string &)>
                   on_result_received_callback)
        : socket_(ios_), work_(std::make_shared<asio::io_context::work>(ios_)),
          deadline_(ios_), body_(INIT_BUF_SIZE),
          client_language_(client_language),
          on_result_received_callback_(std::move(on_result_received_callback))
    {
      thd_ = std::make_shared<std::thread>([this]
                                           { ios_.run(); });
    }

    rpc_client(const std::string &host, unsigned short port)
        : rpc_client(client_language_t::CPP, nullptr, host, port) {}

    rpc_client(client_language_t client_language,
               std::function<void(long, const std::string &)>
                   on_result_received_callback,
               std::string host, unsigned short port)
        : socket_(ios_), work_(std::make_shared<asio::io_context::work>(ios_)),
          deadline_(ios_), host_(std::move(host)), port_(port),
          body_(INIT_BUF_SIZE), client_language_(client_language),
          on_result_received_callback_(std::move(on_result_received_callback))
    {
      thd_ = std::make_shared<std::thread>([this]
                                           { ios_.run(); });
    }

    // 析构函数，实现了完整的client关闭功能，必须通过该析构函数关闭client
    ~rpc_client()
    {
      std::promise<void> promise;
      ios_.post([this, &promise]
                {
                  close();             // 结束所有socket相关的异步任务，关闭连接并释放资源
                  stop_client_ = true; // 设置停止标志
                  std::error_code ec;
                  deadline_.cancel(ec); // 关闭心跳机制
                  promise.set_value();  // 表明异步任务完成，继续执行~rpc_client()
                });

      promise.get_future().wait(); // 阻塞等待异步任务完成，确保所有socket相关的异步任务都结束
      stop();                      // 停止io_context，结束线程
    }

    // 检查 thd_是否可回收，并回收 thd_（所以为什么要叫run？？）
    void run()
    {
      if (thd_ != nullptr && thd_->joinable())
      {
        thd_->join();
      }
    }

    void set_connect_timeout(size_t milliseconds)
    {
      connect_timeout_ = milliseconds;
    }

    void set_reconnect_count(int reconnect_count)
    {
      reconnect_cnt_ = reconnect_count;
    }

    // 同步连接，返回连接是否成功
    bool connect(size_t timeout = 3, bool is_ssl = false)
    {
      if (has_connected_)
        return true;

      assert(port_ != 0);
      if (is_ssl)
      {
        upgrade_to_ssl();
      }
      async_connect(); // 内部会调用 do_read() 方法，等待读取数据
      return wait_conn(timeout);
    }

    // 同步连接，返回连接是否成功
    bool connect(const std::string &host, unsigned short port,
                 bool is_ssl = false, size_t timeout = 3)
    {
      if (port_ == 0)
      {
        host_ = host;
        port_ = port;
      }

      return connect(timeout, is_ssl);
    }

    // 异步连接，公开接口版本
    void async_connect(const std::string &host, unsigned short port)
    {
      if (port_ == 0)
      {
        host_ = host;
        port_ = port;
      }

      async_connect(); // 内部会调用 do_read() 方法，等待读取数据
    }

    // 等待连接状态变为已连接，最多等待timeout秒；返回连接是否成功
    bool wait_conn(size_t timeout)
    {
      if (has_connected_)
      {
        // 已连接，直接返回true
        return true;
      }

      has_wait_ = true;
      std::unique_lock<std::mutex> lock(conn_mtx_);
      // 要么在 async_connect() 成功后被回调函数唤醒，要么超时结束
      bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),
                                        [this]
                                        { return has_connected_.load(); });
      has_wait_ = false;
      // 返回是否在 timeout 时间内连接成功
      return has_connected_;
    }

    // 设置是否自动重连
    // 默认情况下，调用该函数后，会自动重连，并将重连次数设置为最大值
    void enable_auto_reconnect(bool enable = true)
    {
      enable_reconnect_ = enable;
      reconnect_cnt_ = std::numeric_limits<int>::max();
    }

    // 设置是否启动自动心跳机制
    void enable_auto_heartbeat(bool enable = true)
    {
      if (enable)
      {
        // 每5秒发送一次心跳
        // 该心跳包调用 write(0, request_type::req_res, rpc_service::buffer_type(0), 0)
        // 即 req_id = 0, body_size = 0, func_id = 0
        reset_deadline_timer(5);
      }
      else
      {
        // 不启动心跳机制，取消定时器
        deadline_.cancel();
      }
    }

    // 更新server的ip地址和端口
    void update_addr(const std::string &host, unsigned short port)
    {
      host_ = host;
      port_ = port;
    }

    // 该方法可以结束所有正在执行的异步任务，随后关闭连接并释放资源
    void close(bool close_ssl = true)
    {
      asio::error_code ec;
      if (close_ssl)
      {
#ifdef CINATRA_ENABLE_SSL
        if (ssl_stream_)
        {
          ssl_stream_->shutdown(ec);
          ssl_stream_ = nullptr;
        }
#endif
      }

      if (!has_connected_) // 已经关闭，直接返回
        return;

      has_connected_ = false;
      // 关闭 socket
      socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
      socket_.close(ec);
      // 释放资源
      clear_cache();
    }

    // 设置出错时的回调函数 err_cb_
    void set_error_callback(std::function<void(asio::error_code)> f)
    {
      err_cb_ = std::move(f);
    }

    // 返回 temp_req_id_，最后收到的rpc报文的req_id
    uint64_t request_id() { return temp_req_id_; }

    // 返回 has_connected_，连接状态
    bool has_connected() const { return has_connected_; }

    // ------------ 以下4个call()方法用于同步调用rpc服务
    // ------------ 最终调用  future_result<req_result> async_call(const std::string &rpc_name, Args &&...args){...}

    // 返回值为void，并且设置了默认超时时间的同步调用
    template <size_t TIMEOUT, typename T = void, typename... Args>
    typename std::enable_if<std::is_void<T>::value>::type
    call(const std::string &rpc_name, Args &&...args)
    {
      auto future_result =
          async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
      auto status = future_result.wait_for(std::chrono::milliseconds(TIMEOUT));
      if (status == std::future_status::timeout ||
          status == std::future_status::deferred)
      {
        throw std::out_of_range("timeout or deferred"); // 超时或未定义（deferred），抛出异常
      }
      future_result.get().as(); // 检查
    }

    // 返回值为void，并且没有设置超时时间的同步调用
    template <typename T = void, typename... Args>
    typename std::enable_if<std::is_void<T>::value>::type
    call(const std::string &rpc_name, Args &&...args)
    {
      // 调用上一个重载版本，使用默认的超时时间 5s
      call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
    }

    // 返回值不为void，并且设置了默认超时时间的同步调用；没有报错时，以 T 类型返回rpc调用结果
    template <size_t TIMEOUT, typename T, typename... Args>
    typename std::enable_if<!std::is_void<T>::value, T>::type
    call(const std::string &rpc_name, Args &&...args)
    {
      auto future_result =
          async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
      auto status = future_result.wait_for(std::chrono::milliseconds(TIMEOUT));
      if (status == std::future_status::timeout ||
          status == std::future_status::deferred)
      {
        throw std::out_of_range("timeout or deferred");
      }

      return future_result.get().template as<T>();
    }

    // 返回值不为void，并且没有设置超时时间的同步调用；没有报错时，以 T 类型返回rpc调用结果
    template <typename T, typename... Args>
    typename std::enable_if<!std::is_void<T>::value, T>::type
    call(const std::string &rpc_name, Args &&...args)
    {
      // 调用上一个重载版本，使用默认的超时时间 5s
      return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
    }

    // 用于使用future获取rpc调用结果的情况
    // 返回关联的 future_result 结构体（内置了future对象）
    // 会在read服务器发送的报文时，根据报文中的req_id，从future_map_中找到对应的promise对象，并设置其值
    // auto future = client.async_call<CallModel::future>("echo", "purecpp"); ----- 一个可能的调用方式
    template <CallModel model, typename... Args>
    future_result<req_result> async_call(const std::string &rpc_name,
                                         Args &&...args)
    {
      auto p = std::make_shared<std::promise<req_result>>(); // promise会内置一个值/对象，该值可在稍后被设置/赋予，并被关联的future对象获取
      std::future<req_result> future = p->get_future();      // future对象用来获取promise对象设置的值/对象

      uint64_t fu_id = 0;
      {
        std::unique_lock<std::mutex> lock(cb_mtx_);
        fu_id_++;
        fu_id = fu_id_;
        future_map_.emplace(fu_id, std::move(p)); // 将promise对象存入map，key为fu_id
      }

      rpc_service::msgpack_codec codec;
      auto ret = codec.pack_args(std::forward<Args>(args)...); // ret 为序列化的rpc调用的参数
      write(fu_id, request_type::req_res, std::move(ret),
            MD5::MD5Hash32(rpc_name.data()));
      return future_result<req_result>{fu_id, std::move(future)}; // 返回一个包含fu_id和future对象的future_result结构体
    }

    /**
     * This internal_async_call is used for other language client.
     * We use callback to handle the result is received, so we should not
     * add the future to the future map.
     */
    // 使用非c++的client时调用，传入已经序列化的函数名和参数
    // 这种方式不提供future，而是使用回调函数处理rpc调用结果
    long internal_async_call(const std::string &encoded_func_name_and_args)
    {
      // encoded_func_name_and_args同时为函数名和参数列表，因此解析时可能要处理一下
      auto p = std::make_shared<std::promise<req_result>>();
      uint64_t fu_id = 0;
      {
        std::unique_lock<std::mutex> lock(cb_mtx_);
        fu_id_++;
        fu_id = fu_id_;
      }
      msgpack::sbuffer sbuffer;
      sbuffer.write(encoded_func_name_and_args.data(),
                    encoded_func_name_and_args.size());
      write(fu_id, request_type::req_res, std::move(sbuffer),
            MD5::MD5Hash32(encoded_func_name_and_args.data()));
      return fu_id;
    }

    // 用于使用回调函数处理rpc调用结果的情况
    // rpc_name为server上的函数，cb为调用后的回调函数，args为函数参数
    // cb为 void(asio::error_code, string_view) 类型的函数对象，需要在回调函数中使用msgpack的as方法反序列化为指定的类型对象
    template <size_t TIMEOUT = DEFAULT_TIMEOUT, typename... Args>
    void async_call(const std::string &rpc_name,
                    std::function<void(asio::error_code, string_view)> cb,
                    Args &&...args)
    {
      if (!has_connected_)
      {
        // 没有连接，直接调用回调函数，并传入error_code
        if (cb)
          cb(asio::error::make_error_code(asio::error::not_connected),
             "not connected");
        return;
      }

      uint64_t cb_id = 0;
      {
        std::unique_lock<std::mutex> lock(cb_mtx_);
        callback_id_++;
        callback_id_ |= (uint64_t(1) << 63); // 最高位置为1，表明调用rpc的函数
        cb_id = callback_id_;
        auto call = std::make_shared<call_t>(ios_, std::move(cb), TIMEOUT);
        call->start_timer();
        callback_map_.emplace(cb_id, call);
      }

      rpc_service::msgpack_codec codec;
      auto ret = codec.pack_args(std::forward<Args>(args)...); // ret 为序列化的rpc调用的参数
      write(cb_id, request_type::req_res, std::move(ret),
            MD5::MD5Hash32(rpc_name.data()));
    }

    // 销毁 io_context::work 对象，并回收 io_context 所在的线程
    void stop()
    {
      if (thd_ != nullptr)
      {
        work_ = nullptr;
        if (thd_->joinable())
        {
          thd_->join();
        }
        thd_ = nullptr;
      }
    }

    // 订阅主题 key，并使用函数对象 f 处理订阅消息
    // f为 void(string_view) 类型的函数对象
    template <typename Func>
    void subscribe(std::string key, Func f)
    {
      auto it = sub_map_.find(key);
      if (it != sub_map_.end())
      {
        assert("duplicated subscribe");
        return;
      }

      sub_map_.emplace(key, std::move(f));
      send_subscribe(key, "");
      key_token_set_.emplace(std::move(key), "");
    }

    // 订阅主题 (key + token)，并使用函数对象 f 处理订阅消息
    // f为 void(string_view) 类型的函数对象
    template <typename Func>
    void subscribe(std::string key, std::string token, Func f)
    {
      auto composite_key = key + token;
      auto it = sub_map_.find(composite_key);
      if (it != sub_map_.end())
      {
        assert("duplicated subscribe");
        return;
      }

      sub_map_.emplace(std::move(composite_key), std::move(f));
      send_subscribe(key, token);
      key_token_set_.emplace(std::move(key), std::move(token));
    }

    // 发布主题 key，内容为 t
    // client.publish("key", "hello subscriber");  -----  一种可能的调用格式
    template <typename T, size_t TIMEOUT = 3>
    void publish(std::string key, T &&t)
    {
      rpc_service::msgpack_codec codec;
      auto buf = codec.pack(std::move(t)); // buf 存放序列化的数据
      call<TIMEOUT>("publish", std::move(key), "",
                    std::string(buf.data(), buf.size()));
    }

    // 发布主题 (key + token)，内容为 t
    // client1.publish_by_token("key", "sub_key", p); -----  一种可能的调用格式
    template <typename T, size_t TIMEOUT = 3>
    void publish_by_token(std::string key, std::string token, T &&t)
    {
      rpc_service::msgpack_codec codec;
      auto buf = codec.pack(std::move(t)); // buf 存放序列化的数据
      call<TIMEOUT>("publish_by_token", std::move(key), std::move(token),
                    std::string(buf.data(), buf.size()));
    }

#ifdef CINATRA_ENABLE_SSL
    void set_ssl_context_callback(
        std::function<void(asio::ssl::context &)> ssl_context_callback)
    {
      ssl_context_callback_ = std::move(ssl_context_callback);
    }
#endif

  private:
    // 连接 server 的执行函数；注意我们是client哦，只需要保持一个连接哦，不需要再次连接哦
    // 注册异步连接方法 async_connect()，内部调用 do_read() 方法读取报文
    // 所有的公开连接方法都会最终调用该方法
    void async_connect()
    {
      assert(port_ != 0);
      auto addr = asio::ip::address::from_string(host_);
      socket_.async_connect({addr, port_}, [this](const asio::error_code &ec)
                            {
      if (has_connected_ || stop_client_) {
        // has_connected_ 表示已经连接成功，stop_client_表示已经停止了；直接返回
        return;
      }

      if (ec) {
        // 出错处理
        // std::cout << ec.message() << std::endl;

        has_connected_ = false;

        // reconnect_cnt_ 为重连次数
        if (reconnect_cnt_ <= 0) {
          // 重连次数到达上限，直接返回
          return;
        }

        if (reconnect_cnt_ > 0) {
          // 重连次数未到达上限，进行重连
          reconnect_cnt_--;
        }
        // 重连
        async_reconnect();
      } else {
        // std::cout<<"connected ok"<<std::endl;
        // ssl的情况
        if (is_ssl()) {
          handshake();
          return;
        }

        has_connected_ = true;  // 连接成功
        do_read();              // 读取报文
        resend_subscribe();     // 重新订阅
        if (has_wait_){
          // 调用了 wait_conn() 函数，通知其连接已成功
          conn_cond_.notify_one();
        }
          
      } });
    }

    // 重新连接server
    // 调用 reset_socket() 方法重置 socket，然后调用 async_connect() 方法重新连接
    void async_reconnect()
    {
      reset_socket();                                                           // 重置socket
      async_connect();                                                          // 重新连接
      std::this_thread::sleep_for(std::chrono::milliseconds(connect_timeout_)); // 默认睡眠一秒，可能是保证重新连接完成
    }

    // 设置 deadline_ 定时发送心跳信息
    void reset_deadline_timer(size_t timeout)
    {
      if (stop_client_)
      {
        return;
      }

      deadline_.expires_from_now(std::chrono::seconds(timeout));
      deadline_.async_wait([this, timeout](const asio::error_code &ec)
                           {
      if (!ec) {
        if (has_connected_) {
          write(0, request_type::req_res, rpc_service::buffer_type(0), 0);
        }
      }

      reset_deadline_timer(timeout); });
    }

    // message 为序列化后的函数参数，func_id 为md5加密后的函数名
    void write(std::uint64_t req_id, request_type type,
               rpc_service::buffer_type &&message, uint32_t func_id)
    {
      size_t size = message.size();
      assert(size < MAX_BUF_LEN);
      client_message_type msg{req_id, type, {message.release(), size}, func_id}; // 构造报文

      std::unique_lock<std::mutex> lock(write_mtx_);
      outbox_.emplace_back(std::move(msg)); // 放入发送队列
      if (outbox_.size() > 1)
      {
        // 当待发送报文大于于1时，回调函数会自动创建异步任务继续发送，因此无需调用write()
        return;
      }
      write();
    }

    // 真正的发送函数
    // 从 outbox_ 中取出一个报文，构造报文头和报文体，然后调用 async_write() 方法发送
    void write()
    {
      auto &msg = outbox_[0]; // 获取一个报文，类型为 client_message_type
      write_size_ = (uint32_t)msg.content.length();
      std::array<asio::const_buffer, 2> write_buffers;                           // 分别存放rpc报文的头部header和正文body
      header_ = {MAGIC_NUM, msg.req_type, write_size_, msg.req_id, msg.func_id}; // 构造报文头部
      write_buffers[0] = asio::buffer(&header_, sizeof(rpc_header));
      write_buffers[1] = asio::buffer((char *)msg.content.data(), write_size_);

      async_write(write_buffers,
                  [this](const asio::error_code &ec, const size_t length)
                  {
                    if (ec)
                    {
                      // 出错的默认操作，断开连接释放资源、调用error的回调函数
                      has_connected_ = false;
                      close(false);
                      error_callback(ec);

                      return;
                    }

                    std::unique_lock<std::mutex> lock(write_mtx_);
                    if (outbox_.empty())
                    {
                      // 为空，不需要后续操作了，直接返回
                      return;
                    }

                    // 释放当前报文的内存
                    ::free((char *)outbox_.front().content.data());
                    outbox_.pop_front(); // 可见，每当报文发送成功后，才会从发送队列中移除该报文；这使得最多只有一个报文正在发送中

                    if (!outbox_.empty())
                    {
                      // 还有报文待发送，继续调用write()加入异步发送任务
                      this->write();
                    }
                  });
    }

    // 调用 async_read_head() 方法，添加异步读取任务读取rpc报文的head信息
    void do_read()
    {
      async_read_head([this](const asio::error_code &ec, const size_t length)
                      {
      if (!socket_.is_open()) {
        // socket 已经关闭，返回
        // LOG(INFO) << "socket already closed";
        has_connected_ = false;
        return;
      }

      if (!ec) {
        // 读取到head信息，解析head信息，获取body长度
        // 这里不判断一下magic_num是否正确吗？

        // const uint32_t body_len = *((uint32_t*)(head_));
        // auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));
        // auto req_type = *(request_type*)(head_ + sizeof(int32_t) +
        // sizeof(int64_t));
        rpc_header *header = (rpc_header *)(head_); // header指向头部信息
        const uint32_t body_len = header->body_len; // 获取body长度
        if (body_len > 0 && body_len < MAX_BUF_LEN) {
          // 正常情况，继续读取body内容
          if (body_.size() < body_len) {
            body_.resize(body_len);
          }
          read_body(header->req_id, header->req_type, body_len);
          return;
        }

        if (body_len == 0 || body_len > MAX_BUF_LEN) {  // if设置的有问题，这里当 body_len == MAX_BUF_LEN 时没有做处理
          // 处理 body长度异常的情况；关闭连接并释放资源，根据设置决定是否重连

          // LOG(INFO) << "invalid body len";
          close();
          error_callback(
              asio::error::make_error_code(asio::error::message_size));
          return;
        }
      } else {
        // 异步读取出现error，关闭连接并释放资源，调用error_callback()以决定是否重连
        std::cout << ec.message() << "\n";

        {
          std::unique_lock<std::mutex> lock(cb_mtx_);
          for (auto &item : callback_map_) {
            // 因为要关闭连接，所以让所有的回调函数都处理error
            item.second->callback(ec, {});
          }
        }

        close(false);
        error_callback(ec);
      } });
    }

    // 调用 async_read() 方法，添加异步读取任务读取rpc报文的body信息
    void read_body(std::uint64_t req_id, request_type req_type, size_t body_len)
    {
      async_read(body_len, [this, req_id, req_type, body_len](asio::error_code ec, std::size_t length)
                 {
      // cancel_timer();

      if (!socket_.is_open()) {
        // LOG(INFO) << "socket already closed";
        call_back(req_id, asio::error::make_error_code(asio::error::connection_aborted), {}); // 连接已经关闭，调用回调函数处理error
        return;
      }

      if (!ec) {
        // entier body
        if (req_type == request_type::req_res) {
          // req_res类型的rpc请求响应，使用 call_back 调用回调函数 或 设置promise
          call_back(req_id, ec, {body_.data(), body_len});
        } else if (req_type == request_type::sub_pub) {
          // sub_pub类型的rpc请求响应，使用 callback_sub 调用回调函数处理
          callback_sub(ec, {body_.data(), body_len});
        } else {
          // 出错了，关闭连接并释放资源，根据设置决定是否重连
          close();
          error_callback(asio::error::make_error_code(asio::error::invalid_argument));
          return;
        }

        // 回到do_read()状态，继续读取下一个报文
        do_read();
      } else {
        // LOG(INFO) << ec.message();
        has_connected_ = false;
        call_back(req_id, ec, {});
        close();
        error_callback(ec);
      } });
    }

    // 发送订阅请求
    void send_subscribe(const std::string &key, const std::string &token)
    {
      rpc_service::msgpack_codec codec;
      auto ret = codec.pack_args(key, token);
      // 订阅请求的 req_id 为0
      write(0, request_type::sub_pub, std::move(ret), MD5::MD5Hash32(key.data()));
    }

    // 遍历 key_token_set_ ，重新发送每个订阅请求；用于网络断开重连后重新订阅
    void resend_subscribe()
    {
      if (key_token_set_.empty())
        return;

      for (auto &pair : key_token_set_)
      {
        send_subscribe(pair.first, pair.second);
      }
    }

    // 执行 req_id 所对应的回调函数 或 设置promise的值
    void call_back(uint64_t req_id, const asio::error_code &ec,
                   string_view data)
    {
      if (client_language_ == client_language_t::JAVA)
      {
        // For Java client.
        // TODO(qwang): Call java callback.
        // handle error. 这里没有处理error
        on_result_received_callback_(req_id, std::string(data.data(), data.size()));
      }
      else
      {
        // For CPP client.
        temp_req_id_ = req_id;
        auto cb_flag = req_id >> 63;
        if (cb_flag)
        {
          // 最高位为1，为异步调用且没使用future
          std::shared_ptr<call_t> cl = nullptr;
          {
            std::unique_lock<std::mutex> lock(cb_mtx_);
            cl = std::move(callback_map_[req_id]); // 在下方调用erase()删除
          }

          assert(cl); // 要求 req_id 对应的 call_t 对象必须存在
          if (!cl->has_timeout())
          {
            // 没有超时，直接调用回调函数
            cl->cancel();           // 取消定时器
            cl->callback(ec, data); // 调用回调函数
          }
          else
          {
            // 超时了，调用回调函数处理超时error
            cl->callback(asio::error::make_error_code(asio::error::timed_out),
                         {});
          }

          std::unique_lock<std::mutex> lock(cb_mtx_);
          callback_map_.erase(req_id);
        }
        else
        {
          // 最高位为0，使用future
          std::unique_lock<std::mutex> lock(cb_mtx_);
          auto &f = future_map_[req_id];
          if (ec)
          {
            // 出现error
            // LOG<<ec.message();
            if (f)
            {
              // 有关联的 promise 对象存在，存放一个空字符串，并且从future_map_删除
              // std::cout << "invalid req_id" << std::endl;
              f->set_value(req_result{""});
              future_map_.erase(req_id);
              return;
            }
          }
          // 没有error时，要求promise对象必须存在；存放data，并且从future_map_删除
          assert(f);
          f->set_value(req_result{data});
          future_map_.erase(req_id);
        }
      }
    }

    // 解析result，根据key找到对应的回调函数，并调用回调函数处理订阅消息
    void callback_sub(const asio::error_code &ec, string_view result)
    {
      rpc_service::msgpack_codec codec;
      try
      {
        auto tp = codec.unpack<std::tuple<int, std::string, std::string>>(
            result.data(), result.size());
        auto code = std::get<0>(tp);  // result_code::OK
        auto &key = std::get<1>(tp);  // key + token
        auto &data = std::get<2>(tp); // 订阅消息的内容

        auto it = sub_map_.find(key); // 使用 key + token 找到对应的回调函数
        if (it == sub_map_.end())
        {
          return;
        }

        it->second(data); // 调用回调函数处理订阅消息
      }
      catch (const std::exception & /*ex*/)
      {
        error_callback(
            asio::error::make_error_code(asio::error::invalid_argument));
      }
    }

    // 释放 outbox_ 、 callback_map_ 和 future_map_
    void clear_cache()
    {
      {
        // 循环释放 outbox_
        std::unique_lock<std::mutex> lock(write_mtx_);
        while (!outbox_.empty())
        {
          ::free((char *)outbox_.front().content.data());
          outbox_.pop_front();
        }
      }

      {
        // 清空 callback_map_ 和 future_map_
        std::unique_lock<std::mutex> lock(cb_mtx_);
        callback_map_.clear();
        future_map_.clear();
      }
    }

    // 关闭原有socket，重新创建一个并打开
    void reset_socket()
    {
      asio::error_code igored_ec;
      socket_.close(igored_ec);
      socket_ = decltype(socket_)(ios_); // 推断socket_的类型并创建，使用ios_初始化；叼毛，为什么不直接写 socket_ = asio::ip::tcp::socket(ios_);
      if (!socket_.is_open())
      {
        // 确保 socket_ 是打开的
        socket_.open(asio::ip::tcp::v4());
      }
    }

    // call_t 用于封装回调函数，在以回调函数的方式处理异步操作时使用
    // 回调函数必须是 void(asio::error_code, string_view) 类型；内部封装了定时器对象 steady_timer 以实现超时功能
    class call_t : asio::noncopyable,
                   public std::enable_shared_from_this<call_t>
    {
    public:
      call_t(asio::io_service &ios,
             std::function<void(asio::error_code, string_view)> cb,
             size_t timeout)
          : timer_(ios), cb_(std::move(cb)), timeout_(timeout) {}

      // 设置超时器，当超时后将会设置 has_timeout_ 为 true
      void start_timer()
      {
        if (timeout_ == 0)
        {
          return;
        }

        timer_.expires_from_now(std::chrono::milliseconds(timeout_));
        auto self = this->shared_from_this();
        timer_.async_wait([this, self](asio::error_code ec)
                          {
        if (ec) {
          return;
        }

        has_timeout_ = true; });
      }

      // 调用内部封装的回调函数
      void callback(asio::error_code ec, string_view data) { cb_(ec, data); }

      // 是否超时了
      bool has_timeout() const { return has_timeout_; }

      // 取消定时器 steady_timer
      void cancel()
      {
        if (timeout_ == 0)
        {
          return;
        }

        asio::error_code ec;
        timer_.cancel(ec);
      }

    private:
      asio::steady_timer timer_;
      std::function<void(asio::error_code, string_view)> cb_;
      size_t timeout_;
      bool has_timeout_ = false;
    };

    // 异步操作中，出现error时的回调函数：将会调用 err_cb_() 处理错误信息，并调用 async_reconnect() 重连（enable_reconnect_ 为 true）
    void error_callback(const asio::error_code &ec)
    {
      if (err_cb_)
      {
        // 默认的错误回调函数为 async_reconnect() ，即出错时，会注册一个异步重连操作
        err_cb_(ec);
      }

      if (enable_reconnect_)
      {
        async_reconnect();
      }
    }

    // 设置默认的错误回调函数；默认的错误回调函数是 async_reconnect()
    // 该函数在出错时，会注册一个异步重连操作
    void set_default_error_cb()
    {
      err_cb_ = [this](asio::error_code)
      { async_connect(); };
    }

    // 判断是否是 ssl 连接
    bool is_ssl() const
    {
#ifdef CINATRA_ENABLE_SSL
      return ssl_stream_ != nullptr;
#else
      return false;
#endif
    }

    // ssl中的握手操作
    void handshake()
    {
#ifdef CINATRA_ENABLE_SSL
      ssl_stream_->async_handshake(asio::ssl::stream_base::client,
                                   [this](const asio::error_code &ec)
                                   {
                                     if (!ec)
                                     {
                                       has_connected_ = true;
                                       do_read();
                                       resend_subscribe();
                                       if (has_wait_)
                                         conn_cond_.notify_one();
                                     }
                                     else
                                     {
                                       error_callback(ec);
                                       close();
                                     }
                                   });
#endif
    }

    // ssl相关
    void upgrade_to_ssl()
    {
#ifdef CINATRA_ENABLE_SSL
      if (ssl_stream_)
        return;

      asio::ssl::context ssl_context(asio::ssl::context::sslv23);
      ssl_context.set_default_verify_paths();
      asio::error_code ec;
      ssl_context.set_options(asio::ssl::context::default_workarounds, ec);
      if (ssl_context_callback_)
      {
        ssl_context_callback_(ssl_context);
      }
      ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket &>>(
          socket_, ssl_context);
      // verify peer TODO
#else
      assert(is_ssl()); // please add definition CINATRA_ENABLE_SSL, not allowed
                        // coming in this branch
#endif
    }

    // 调用 asio::async_read 读取head信息，存入 head_ 中
    template <typename Handler>
    void async_read_head(Handler handler)
    {
      if (is_ssl())
      {
#ifdef CINATRA_ENABLE_SSL
        asio::async_read(*ssl_stream_, asio::buffer(head_, HEAD_LEN),
                         std::move(handler));
#endif
      }
      else
      {
        asio::async_read(socket_, asio::buffer(head_, HEAD_LEN),
                         std::move(handler));
      }
    }

    // 调用 asio::async_read 读取body信息，存入 body_ 中
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
        asio::async_read(socket_, asio::buffer(body_.data(), size_to_read),
                         std::move(handler));
      }
    }

    // 调用 asio::async_write，注册异步写任务并设置回调函数
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
        asio::async_write(socket_, buffers, std::move(handler));
      }
    }

    asio::io_context ios_;         // io context，asio库异步操作的核心对象
    asio::ip::tcp::socket socket_; // socket，用于与server的网络通信
#ifdef CINATRA_ENABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket &>> ssl_stream_;
    std::function<void(asio::ssl::context &)> ssl_context_callback_;
#endif
    std::shared_ptr<asio::io_context::work> work_; // ios_ 对应的work对象，保证ios_一直运行
    std::shared_ptr<std::thread> thd_ = nullptr;   // ios_ 的运行线程（可以看出客户端的异步操作是单线程的）

    std::string host_;                         // server ip
    unsigned short port_ = 0;                  // server 端口
    size_t connect_timeout_ = 1000;            // 单位为秒；重连的间隔时间
    int reconnect_cnt_ = -1;                   // 重连次数，小于等于0时停止重连，关闭客户端
    std::atomic_bool has_connected_ = {false}; // 是否已经连接，原子类型，保证线程安全
    std::mutex conn_mtx_;                      // 连接的互斥锁
    std::condition_variable conn_cond_;        // 用于异步连接中的超时设置，调用 wait_for() 方法等待连接成功或超时
    bool has_wait_ = false;                    // 是否正在等待连接，与 conn_cond_ 配合使用

    asio::steady_timer deadline_; // ios_ 对应的定时器，用于心跳机制
    bool stop_client_ = false;    // 是否停止client；析构函数被执行时会被设置为true

    // rpc报文结构体 ，并不是最终发送的报文，仅作为中间参数传递使用
    struct client_message_type
    {
      std::uint64_t req_id;
      request_type req_type;
      string_view content;
      uint32_t func_id;
    };
    std::deque<client_message_type> outbox_;       // 存放待发送的rpc报文
    uint32_t write_size_ = 0;                      // 当前待发送的rpc报文的 body（序列化的函数参数或订阅key+token） 大小
    std::mutex write_mtx_;                         // 写锁，保证写报文时 outbox_ 的线程安全
    uint64_t fu_id_ = 0;                           // 以 future 方式调用rpc时，使用的id
    std::function<void(asio::error_code)> err_cb_; // 出现error时，处理error的回调函数
    bool enable_reconnect_ = false;                // 控制error出现时，是否重新链接server

    std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<req_result>>>
        future_map_;                                                          // fu_id_ -> promise<req_result> ； 用于线程间的值传递等，关联的future可以在promise有可用的值后，获取该值
    std::unordered_map<std::uint64_t, std::shared_ptr<call_t>> callback_map_; // callback_id_ -> call_t（封装处理rpc调用结果的callback） ；会在 async_call() 函数中加入回调函数
    std::mutex cb_mtx_;                                                       // 保证 future_map_ 、 callback_map_ 、 callback_id_ 等的线程安全
    uint64_t callback_id_ = 0;                                                // 使用回调函数处理异步结果时，使用的id

    uint64_t temp_req_id_ = 0; // 最后一个接收的rpc报文的req_id；当调用 call_back() 处理最后的一个rpc调用结果时，设置temp_req_id_为req_id

    char head_[HEAD_LEN] = {}; // 暂存接收到的rpc报文的头部
    std::vector<char> body_;   // 暂存接收到的rpc报文的正文

    rpc_header header_; // 存放当前正在发送的 rpc报文头部

    std::unordered_map<std::string, std::function<void(string_view)>> sub_map_; // maybe key -> func, or (key + token) -> func
    std::set<std::pair<std::string, std::string>> key_token_set_;               // key -> token

    client_language_t client_language_ = client_language_t::CPP;                 // cilent 的语言
    std::function<void(long, const std::string &)> on_result_received_callback_; // 非c++的调用结果回调函数；构造时传入，在收到调用结果时调用
  };
} // namespace rest_rpc
