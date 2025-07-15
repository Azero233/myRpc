#ifndef REST_RPC_IO_SERVICE_POOL_H_
#define REST_RPC_IO_SERVICE_POOL_H_

#include "use_asio.hpp"
#include <memory>
#include <vector>

// 这个文件负责创建io_context(io_service)池，并调用其run()方法；对于每一个io_context，都会创建一个线程来调用其run()方法

namespace rest_rpc
{
  namespace rpc_service
  {
    class io_service_pool : private asio::noncopyable
    {
    public:
      // 构造函数，根据传入的pool_size创建相应数量的io_service对象和work对象
      explicit io_service_pool(std::size_t pool_size) : next_io_service_(0)
      {
        // 如果io_service池大小为0，抛出异常
        if (pool_size == 0)
          throw std::runtime_error("io_service_pool size is 0");

        for (std::size_t i = 0; i < pool_size; ++i)
        {
          // 创建io_service对象和work对象，并将它们添加到io_services_和work_中
          io_service_ptr io_service(new asio::io_context);
          work_ptr work(new asio::io_context::work(*io_service));
          io_services_.push_back(io_service);
          work_.push_back(work);
        }
      }

      // 循环创建线程，每个线程对应一个io_context对象，并调用其run()方法
      void run()
      {
        // 这里虽然使用了shared_ptr，但由于外界获取不了线程对象，其实不使用也可以
        std::vector<std::shared_ptr<std::thread>> threads;
        for (std::size_t i = 0; i < io_services_.size(); ++i)
        {
          // 循环创建线程，每个线程对应一个io_context对象，并调用其run()方法
          threads.emplace_back(std::make_shared<std::thread>(
              [](io_service_ptr svr)
              { svr->run(); }, io_services_[i]));
        }
        // 每个线程仅可以使用一次，这里将每个线程回收
        for (std::size_t i = 0; i < threads.size(); ++i)
          threads[i]->join();
      }

      // 调用每一个io_context对象的stop()方法
      void stop()
      {
        for (std::size_t i = 0; i < io_services_.size(); ++i)
        {
          // 通知io_context的运行循环（通常是通过调用io_context.run()启动的）退出。
          // 这意味着，一旦当前正在处理的异步操作完成，io_context将停止处理新的异步操作，并且io_context.run()将返回。
          io_services_[i]->stop();
        }
      }

      // 循环的方式获取一个io_service对象
      asio::io_context &get_io_service()
      {
        asio::io_context &io_service = *io_services_[next_io_service_];
        ++next_io_service_;
        if (next_io_service_ == io_services_.size())
          next_io_service_ = 0;
        return io_service;
      }

    private:
      typedef std::shared_ptr<asio::io_context> io_service_ptr;
      typedef std::shared_ptr<asio::io_context::work> work_ptr;

      /// The pool of io_services.
      std::vector<io_service_ptr> io_services_;

      /// The work that keeps the io_services running.
      std::vector<work_ptr> work_;  // 创建 asio::io_context::work 对象时，它会保持 io_context 运行，直到 work 对象被销毁。这确保了在 work 对象的生命周期内，io_context 会持续运行，处理注册到它上面的异步操作。

      /// The next io_service to use for a connection.
      std::size_t next_io_service_;
    };
  } // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_IO_SERVICE_POOL_H_