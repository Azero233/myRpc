#ifndef REST_RPC_RPC_SERVER_H_
#define REST_RPC_RPC_SERVER_H_

#include "connection.h"
#include "io_service_pool.h"
#include "router.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>

using asio::ip::tcp;
using std::is_function;

namespace rest_rpc
{
  namespace rpc_service
  {
    using rpc_conn = std::weak_ptr<connection>;
    class rpc_server : private asio::noncopyable
    {
    public:
      // 构造函数，传入端口号port，线程池大小size，超时时间timeout_seconds，检查时间check_seconds
      rpc_server(unsigned short port, size_t size, size_t timeout_seconds = 15,
                 size_t check_seconds = 10)
          : io_service_pool_(size), acceptor_(io_service_pool_.get_io_service(),
                                              tcp::endpoint(tcp::v4(), port)),
            timeout_seconds_(timeout_seconds), check_seconds_(check_seconds),
            signals_(io_service_pool_.get_io_service())
      {
        do_accept();
        // check_thread_指向的线程负责清除超时的连接
        check_thread_ = std::make_shared<std::thread>([this]
                                                      { clean(); });
        pub_sub_thread_ =
            std::make_shared<std::thread>([this]
                                          { clean_sub_pub(); });

        // 设置信号处理，注册信号回调函数
        signals_.add(SIGINT);
        signals_.add(SIGTERM);
#if defined(SIGQUIT)
        signals_.add(SIGQUIT);
#endif // defined(SIGQUIT)
        do_await_stop();
      }

      // 这个构造函数在ssl情况下调用，这里我们不使用
      rpc_server(unsigned short port, size_t size, ssl_configure ssl_conf,
                 size_t timeout_seconds = 15, size_t check_seconds = 10)
          : rpc_server(port, size, timeout_seconds, check_seconds)
      {
#ifdef CINATRA_ENABLE_SSL
        ssl_conf_ = std::move(ssl_conf);
#else
        assert(false); // please add definition CINATRA_ENABLE_SSL, not allowed
                       // coming in this branch
#endif
      }

      // 析构函数，直接调用stop()
      ~rpc_server() { stop(); }

      // 启动io_context池（用thd_管理 io_service_pool_）
      void async_run()
      {
        thd_ = std::make_shared<std::thread>([this]
                                             { io_service_pool_.run(); });
      }

      // 启动io_context池（以当前线程管理 io_service_pool_）
      void run() { io_service_pool_.run(); }

      // 注册一个rpc函数，函数名是name，函数是f
      template <bool is_pub = false, typename Function>
      void register_handler(std::string const &name, const Function &f)
      {
        router_.register_handler<is_pub>(name, f);
      }

      // 注册一个rpc函数，函数名是name，函数是f，函数所属对象是self
      template <bool is_pub = false, typename Function, typename Self>
      void register_handler(std::string const &name, const Function &f, Self *self)
      {
        router_.register_handler<is_pub>(name, f, self);
      }

      // 设置返回错误码时的回调函数
      void set_error_callback(std::function<void(asio::error_code, string_view)> f)
      {
        err_cb_ = std::move(f);
      }

      // 设置连接超时回调函数
      void set_conn_timeout_callback(std::function<void(int64_t)> callback)
      {
        conn_timeout_callback_ = std::move(callback);
      }

      // 设置网络传输错误时的回调函数 on_net_err_callback_
      void set_network_err_callback(
          std::function<void(std::shared_ptr<connection>, std::string /*reason*/)>
              on_net_err)
      {
        on_net_err_callback_ = std::move(on_net_err);
      }

      // 发布消息，通过key找到订阅者，data为消息内容
      template <typename T>
      void publish(const std::string &key, T data)
      {
        publish(key, "", std::move(data));
      }

      // 发布消息，通过key + token的组合找到订阅者，data为消息内容
      template <typename T>
      void publish_by_token(const std::string &key, std::string token, T data)
      {
        publish(key, std::move(token), std::move(data));
      }

      // 获取 token_list_ （拷贝返回）
      std::set<std::string> get_token_list()
      {
        std::unique_lock<std::mutex> lock(sub_mtx_);
        return token_list_;
      }

    private:
      // 创建一个connection对象并设置回调函数（负责添加订阅者），等待并管理下一个到来的连接
      void do_accept()
      {
        // 新建一个conn_的指针（原对象在connections_中，不会因为reset被释放），从io_service_pool_中取出一个io_service对象以创建新的连接对象
        // 基于同一个io_service对象创建的连接对象，他们执行在同一个线程上执行异步操作
        conn_.reset(new connection(io_service_pool_.get_io_service(),
                                   timeout_seconds_, router_));

        // 设置conn_的回调函数，该函数负责添加订阅者；当收到rpc的订阅发布请求时，会调用该函数，将订阅者添加到sub_map_中
        conn_->set_callback([this](std::string key, std::string token,
                                   std::weak_ptr<connection> conn)
                            {
          // sub_map_加入订阅者，token_list_中加入token
          std::unique_lock<std::mutex> lock(sub_mtx_);
          sub_map_.emplace(std::move(key) + token, conn);
          if (!token.empty()) {
            token_list_.emplace(std::move(token));
          } }); // ---end of conn_->set_callback

        // 当连接事件到达acceptor_监听的端口时，调用async_accept指定的回调函数，由conn_管理该连接
        acceptor_.async_accept(conn_->socket(), [this](asio::error_code ec)
                               {
          if (!acceptor_.is_open()) {
            // 若acceptor_已关闭，直接返回，不建立连接
            return;
          }

          if (ec) {
            // LOG(INFO) << "acceptor error: " <<
            // ec.message();
          } else {
    #ifdef CINATRA_ENABLE_SSL
            if (!ssl_conf_.cert_file.empty()) {
              conn_->init_ssl_context(ssl_conf_);
            }
    #endif

            // in else
            if (on_net_err_callback_) {
              // 若设置了on_net_err_callback_，为conn_设置该回调函数
              conn_->on_network_error(on_net_err_callback_);
            }
            conn_->start(); // 启动服务
            std::unique_lock<std::mutex> lock(mtx_);
            // 设置连接id，并加入connections_哈希表
            conn_->set_conn_id(conn_id_);
            connections_.emplace(conn_id_++, conn_);
          }

          // 再次调用do_accept()函数，以继续监听端口
          do_accept(); }); // ---end of acceptor_.async_accept
      }

      // 清除关闭/超时的连接
      void clean()
      {
        // 未调用stop()函数，继续循环
        while (!stop_check_) // 关闭时，这个位置小概率会跳过导致没有释放连接；可以考虑while下方无条件再清理一次连接
        {
          std::unique_lock<std::mutex> lock(mtx_);
          // 每 check_seconds_ 秒清理一次连接，或者被stop()函数唤醒，清理全部连接
          cv_.wait_for(lock, std::chrono::seconds(check_seconds_));
          // 开始清理连接
          for (auto it = connections_.cbegin(); it != connections_.cend();)
          {
            if (it->second->has_closed())
            {
              // 当前connection关闭了，调用回调函数conn_timeout_callback_(如果有)，并从哈希表中移除
              if (conn_timeout_callback_)
              {
                conn_timeout_callback_(it->second->conn_id());
              }
              it = connections_.erase(it);
            }
            else
            {
              // 当前connection没有关闭，检查下一个
              ++it;
            }
          }
        }
      }

      // 订阅发布系统的清理功能，清理订阅了已经关闭的连接的token
      // token不唯一的情况下，会导致当一个使用了token A的连接断开后，该token A会从token_list_中移除，然而可能token B仍使用该token，导致token_list_不一致
      // 不过，尽管如此，目前内置函数不会因为token_list_而出现bug，但是 std::set<std::string> get_token_list() 方法会出现问题
      void clean_sub_pub()
      {
        // 未调用stop()函数，继续循环
        while (!stop_check_pub_sub_)
        {
          std::unique_lock<std::mutex> lock(sub_mtx_);
          // 等待10秒，或者直接被stop()函数唤醒
          sub_cv_.wait_for(lock, std::chrono::seconds(10));

          for (auto it = sub_map_.cbegin(); it != sub_map_.cend();)
          {
            // it为订阅者列表的迭代器，it->first为订阅的key + token，it->second为订阅者
            auto conn = it->second.lock();
            if (conn == nullptr || conn->has_closed())
            {
              // 连接对象已经被销毁（即 conn == nullptr）或者连接已经关闭
              // remove token
              for (auto t = token_list_.begin(); t != token_list_.end();)
              {
                // t为token_list_中的一个token
                if (it->first.find(*t) != std::string::npos)
                {
                  // 找到token，删除并移动到下一个token
                  t = token_list_.erase(t);
                }
                else
                {
                  ++t;
                }
              }

              it = sub_map_.erase(it);
            }
            else
            {
              // 连接对象存在且未关闭，继续检查下一个
              ++it;
            }
          }
        }
      }

      // 检查 err_cb_ 是否为空，不为空则调用
      void error_callback(const asio::error_code &ec, string_view msg)
      {
        if (err_cb_)
        {
          err_cb_(ec, msg);
        }
      }

      // 发布订阅消息，data为向订阅了key + token的连接发布的数据
      template <typename T>
      void publish(std::string key, std::string token, T data)
      {
        {
          std::unique_lock<std::mutex> lock(sub_mtx_);
          // 订阅为空，直接返回
          if (sub_map_.empty())
            return;
        }

        std::shared_ptr<std::string> shared_data =
            get_shared_data<T>(std::move(data));        // 若data可以赋值给string，则shared_data指向data，否则将data序列化并令shared_data指向序列化后的数据
        std::unique_lock<std::mutex> lock(sub_mtx_);    // 有意思吗？上面加锁后直接释放了，这里还要加锁
        auto range = sub_map_.equal_range(key + token); // 获取订阅了key + token的连接的迭代器范围；equal_range传入一个key，返回一个pair，first指向第一个key的迭代器，second指向最后一个key的迭代器+1
        if (range.first != range.second)
        {
          // 不相等说明存在订阅 key + token 的连接
          for (auto it = range.first; it != range.second; ++it)
          {
            auto conn = it->second.lock(); // conn 为 指向订阅连接对象的 shared_ptr
            if (conn == nullptr || conn->has_closed())
            {
              continue;
            }
            // 发布数据；connection 的 publish 方法将 shared_data 发送给订阅者，该方法的req_id为0，表示这是一个发布消息
            conn->publish(key + token, *shared_data);
          }
        }
        else
        {
          // 相等，说明不存在订阅者
          error_callback(
              asio::error::make_error_code(asio::error::invalid_argument),
              "The subscriber of the key: " + key + " does not exist.");
        }
      }

      // 当 data 可以赋值给string类型的变量时使用，返回指向data的shared_ptr<string>
      template <typename T>
      typename std::enable_if<std::is_assignable<std::string, T>::value,
                              std::shared_ptr<std::string>>::type
      get_shared_data(std::string data)
      {
        return std::make_shared<std::string>(std::move(data));
      }

      // 当 data 不能赋值给string类型的变量时使用，将data序列化后返回指向序列化数据的shared_ptr<string>
      template <typename T>
      typename std::enable_if<!std::is_assignable<std::string, T>::value,
                              std::shared_ptr<std::string>>::type
      get_shared_data(T data)
      {
        msgpack_codec codec;
        auto buf = codec.pack(std::move(data));
        return std::make_shared<std::string>(buf.data(), buf.size());
      }

      // 注册信号回调函数；当预设的信号发出时，调用stop()方法
      void do_await_stop()
      {
        // signals_.async_wait(signal_handler) 的作用在于，当预设的信号发出时，调用传入的回调函数signal_handler
        signals_.async_wait(
            [this](std::error_code /*ec*/, int /*signo*/)
            { stop(); });
      }

      // 关闭rpc服务器程序
      void stop()
      {
        // 已关闭，直接返回
        if (has_stoped_)
        {
          return;
        }

        {
          std::unique_lock<std::mutex> lock(mtx_);
          stop_check_ = true;
          // 关闭所有连接
          cv_.notify_all();
        }
        // 回收 清理连接的线程(clean()函数)
        check_thread_->join();

        {
          std::unique_lock<std::mutex> lock(sub_mtx_);
          stop_check_pub_sub_ = true;
          // 关闭所有订阅
          sub_cv_.notify_all();
        }
        // 回收 清理订阅线程(clean_sub_pub()函数)
        pub_sub_thread_->join();

        // 关闭io_service_pool，新的异步操作将被拒绝
        io_service_pool_.stop();
        if (thd_)
        {
          // 回收thd_
          thd_->join();
        }
        // 标志rpc服务器已关闭
        has_stoped_ = true;
      }

      io_service_pool io_service_pool_;
      tcp::acceptor acceptor_;           // 用于监听客户端连接
      std::shared_ptr<connection> conn_; // 用于管理下一个到来连接的connection指针
      std::shared_ptr<std::thread> thd_; // 若使用，则用于运行io_service_pool（不使用时，使用当前线程运行io_service_pool）
      std::size_t timeout_seconds_;      // 连接超时时间

      std::unordered_map<int64_t, std::shared_ptr<connection>> connections_; // key: std::shared_ptr<connection>
      int64_t conn_id_ = 0;                                                  // 当前 conn_ 的连接id
      std::mutex mtx_;                                                       // connections_的互斥锁
      std::shared_ptr<std::thread> check_thread_;                            // 用于清理连接的线程，调用clean()函数
      size_t check_seconds_;                                                 // clean()中，条件变量cv_的等待时间，未被唤醒就等待check_seconds_秒
      bool stop_check_ = false;                                              // 为true时，表明运行了stop()函数，关闭rpc服务，开始关闭所有连接
      std::condition_variable cv_;                                           // 连接清理的条件变量

      asio::signal_set signals_; // 用于在io_context上注册信号处理函数；默认情况下，注册的信号是SIGINT和SIGTERM，回调函数为stop()

      std::function<void(asio::error_code, string_view)> err_cb_; // 处理返回错误码的回调函数
      std::function<void(int64_t)> conn_timeout_callback_;        // 处理连接超时的回调函数，参数为某个connection对象的connid
      std::function<void(std::shared_ptr<connection>, std::string)>
          on_net_err_callback_ = nullptr;                                       // 处理网络错误的回调函数
      std::unordered_multimap<std::string, std::weak_ptr<connection>> sub_map_; // 寻找订阅的连接；key: key + token，value：std::weak_ptr<connection>；值得一提的是这里使用了weak_ptr管理连接
      std::set<std::string> token_list_;                                        // 记录订阅的token
      std::mutex sub_mtx_;                                                      // sub_map_的互斥锁
      std::condition_variable sub_cv_;                                          // 订阅清理的条件变量

      std::shared_ptr<std::thread> pub_sub_thread_; // 用于清理订阅的线程，调用clean_sub()函数
      bool stop_check_pub_sub_ = false;             // 为true时，表明运行了stop()函数，关闭rpc服务，开始关闭所有连接

      ssl_configure ssl_conf_;                // ssl配置，用不到
      router router_;                         // 管理rpc服务的函数注册、执行和删除
      std::atomic_bool has_stoped_ = {false}; // rpc服务器是否已经关闭
    };
  } // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_RPC_SERVER_H_
