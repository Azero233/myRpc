#include <rest_rpc.hpp>
using namespace rest_rpc;
using namespace rpc_service;
#include <fstream>

#include "qps.h"

struct dummy {
  int add(rpc_conn conn, int a, int b) {
    // using rpc_conn = std::weak_ptr<connection>;   shared_conn 为 shared_ptr<connection>
    auto shared_conn = conn.lock();
    if (shared_conn) {
      // 检查 shared_conn 是否为空；避免connection对象已经被销毁的情况
      shared_conn->set_user_data(std::string("aa"));
      auto s = conn.lock()->get_user_data<std::string>();
      std::cout << s << '\n'; // aa；connection对象可以存放任意类型的用户数据
    }

    return a + b;
  }
};

std::string translate(rpc_conn conn, const std::string &orignal) {
  std::string temp = orignal;
  for (auto &c : temp) {
    c = std::toupper(c);
  }
  return temp;
}

void hello(rpc_conn conn, const std::string &str) {
  std::cout << "hello " << str << std::endl;
}

struct person {
  int id;
  std::string name;
  int age;

  MSGPACK_DEFINE(id, name, age);
};

std::string get_person_name(rpc_conn conn, const person &p) { return p.name; }

person get_person(rpc_conn conn) { return {1, "tom", 20}; }

void upload(rpc_conn conn, const std::string &filename,
            const std::string &content) {
  std::cout << content.size() << std::endl;
  std::ofstream file(filename, std::ios::binary);
  file.write(content.data(), content.size());
}

std::string download(rpc_conn conn, const std::string &filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return "";
  }

  file.seekg(0, std::ios::end);
  size_t file_len = file.tellg();
  file.seekg(0, std::ios::beg);
  std::string content;
  content.resize(file_len);
  file.read(&content[0], file_len);
  std::cout << file_len << std::endl;

  return content;
}

qps g_qps;

std::string get_name(rpc_conn conn, const person &p) {
  g_qps.increase();
  return p.name;
}

// if you want to response later, you can use async model, you can control when
// to response
void delay_echo(rpc_conn conn, const std::string &src) {
  auto sp = conn.lock();
  sp->set_delay(true);
  auto req_id = sp->request_id(); // note: you need keep the request id at that
  // time, and pass it into the async thread
  std::thread thd([conn, req_id, src] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto conn_sp = conn.lock();
    if (conn_sp) {
      conn_sp->pack_and_response(req_id, std::move(src));
    }
  });
  thd.detach();
}

std::string echo(rpc_conn conn, const std::string &src) {
  g_qps.increase();
  return src;
}

int get_int(rpc_conn conn, int val) { return val; }

void test_ssl() {
  rpc_server server(9000, std::thread::hardware_concurrency(),
                    {"server.crt", "server.key"});
  server.register_handler("hello", hello);
  server.register_handler("echo", echo);
  server.run();
}

void benchmark_test() {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("echo", echo);
  server.run();
}

struct dummy1 {
  size_t id;
  std::wstring str;
  MSGPACK_DEFINE(id, str);
};

dummy1 get_dummy(rpc_conn conn, dummy1 d) { return d; }

int main() {
  //  benchmark_test();
  // std::thread::hardware_concurrency() 系统支持的硬件并发线程数
  rpc_server server(9000, std::thread::hardware_concurrency(), 3600);

  dummy d;
  server.register_handler("add", &dummy::add, &d);

  server.register_handler("get_dummy", get_dummy);

  server.register_handler("translate", translate);
  server.register_handler("hello", hello);
  server.register_handler("get_person_name", get_person_name);
  server.register_handler("get_person", get_person);
  server.register_handler("upload", upload);
  server.register_handler("download", download);
  server.register_handler("get_name", get_name);
  server.register_handler("delay_echo", delay_echo);
  server.register_handler("echo", echo);
  server.register_handler("get_int", get_int);

  // client 可以调用 publish_by_token 和 publish 函数，向其他的client发送订阅的消息
  server.register_handler("publish_by_token", [&server](rpc_conn conn,
                                                        std::string key,
                                                        std::string token,
                                                        std::string val) {
    server.publish_by_token(std::move(key), std::move(token), std::move(val));
  });

  server.register_handler("publish",
                          [&server](rpc_conn conn, std::string key,
                                    std::string token, std::string val) {
                            server.publish(std::move(key), std::move(val));
                          });
  server.set_network_err_callback(
      [](std::shared_ptr<connection> conn, std::string reason) {
        std::cout << "remote client address: " << conn->remote_address()
                  << " networking error, reason: " << reason << "\n";
      });

  bool stop = false;

  // 用于订阅发布的线程对象
  std::thread thd([&server, &stop] {
    person p{1, "tom", 20};
    while (!stop) {
      // 向所有订阅了 key 的客户端发布消息 “hello subscriber”
      server.publish("key", "hello subscriber");
      auto list = server.get_token_list();
      for (auto &token : list) {
        // 遍历token列表，向订阅了 key+token 和 key1+token 的客户端发布消息
        server.publish_by_token("key", token, p);
        server.publish_by_token("key1", token, "hello subscriber1");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  server.run();
  stop = true;
  thd.join();
}