
#include <iostream>

//configuration
#define ST_ASIO_SERVER_PORT		9527
#define ST_ASIO_ASYNC_ACCEPT_NUM 5
#define ST_ASIO_REUSE_OBJECT //use objects pool
//#define ST_ASIO_FREE_OBJECT_INTERVAL 60 //it's useless if ST_ASIO_REUSE_OBJECT macro been defined
//#define ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER //force to use the msg recv buffer
#define ST_ASIO_ENHANCED_STABILITY
//#define ST_ASIO_FULL_STATISTIC //full statistic will slightly impact efficiency
//#define ST_ASIO_USE_STEADY_TIMER
//#define ST_ASIO_USE_SYSTEM_TIMER

//use the following macro to control the type of packer and unpacker
#define PACKER_UNPACKER_TYPE	0
//0-default packer and unpacker, head(length) + body
//1-default replaceable_packer and replaceable_unpacker, head(length) + body
//2-fixed length unpacker
//3-prefix and suffix packer and unpacker

#if 1 == PACKER_UNPACKER_TYPE
#define ST_ASIO_DEFAULT_PACKER replaceable_packer<>
#define ST_ASIO_DEFAULT_UNPACKER replaceable_unpacker<>
#elif 2 == PACKER_UNPACKER_TYPE
#define ST_ASIO_DEFAULT_PACKER fixed_length_packer
#define ST_ASIO_DEFAULT_UNPACKER fixed_length_unpacker
#elif 3 == PACKER_UNPACKER_TYPE
#define ST_ASIO_DEFAULT_PACKER prefix_suffix_packer
#define ST_ASIO_DEFAULT_UNPACKER prefix_suffix_unpacker
#endif
//configuration

#include "../include/ext/st_asio_wrapper_server.h"
using namespace st_asio_wrapper;
using namespace st_asio_wrapper::ext;

#define QUIT_COMMAND	"quit"
#define RESTART_COMMAND	"restart"
#define LIST_ALL_CLIENT	"list_all_client"
#define LIST_STATUS		"status"
#define SUSPEND_COMMAND	"suspend"
#define RESUME_COMMAND	"resume"

//demonstrate how to use custom packer
//under the default behavior, each st_tcp_socket has their own packer, and cause memory waste
//at here, we make each echo_socket use the same global packer for memory saving
//notice: do not do this for unpacker, because unpacker has member variables and can't share each other
auto global_packer(boost::make_shared<ST_ASIO_DEFAULT_PACKER>());

//about congestion control
//
//in 1.3, congestion control has been removed (no post_msg nor post_native_msg anymore), this is because
//without known the business (or logic), framework cannot always do congestion control properly.
//now, users should take the responsibility to do congestion control, there're two ways:
//
//1. for receiver, if you cannot handle msgs timely, which means the bottleneck is in your business,
//    you should open/close congestion control intermittently;
//   for sender, send msgs in on_msg_send() or use sending buffer limitation (like safe_send_msg(..., false)),
//    but must not in service threads, please note.
//
//2. for sender, if responses are available (like pingpong test), send msgs in on_msg()/on_msg_handle().
//    this will reduce IO throughput, because SOCKET's sliding window is not fully used, pleae note.
//
//asio_server chose method #1

//demonstrate how to control the type of st_server_socket_base::server from template parameter
class i_echo_server : public i_server
{
public:
	virtual void test() = 0;
};

class echo_socket : public st_server_socket_base<ST_ASIO_DEFAULT_PACKER, ST_ASIO_DEFAULT_UNPACKER, i_echo_server>
{
public:
	echo_socket(i_echo_server& server_) : st_server_socket_base(server_)
	{
		inner_packer(global_packer);

#if 2 == PACKER_UNPACKER_TYPE
		boost::dynamic_pointer_cast<ST_ASIO_DEFAULT_UNPACKER>(inner_unpacker())->fixed_length(1024);
#elif 3 == PACKER_UNPACKER_TYPE
		boost::dynamic_pointer_cast<ST_ASIO_DEFAULT_UNPACKER>(inner_unpacker())->prefix_suffix("begin", "end");
#endif
	}

public:
	//because we use objects pool(REUSE_OBJECT been defined), so, strictly speaking, this virtual
	//function must be rewrote, but we don't have member variables to initialize but invoke father's
	//reset() directly, so, it can be omitted, but we keep it for possibly future using
	virtual void reset() {st_server_socket_base::reset();}

protected:
	virtual void on_recv_error(const boost::system::error_code& ec)
	{
		//the type of st_server_socket_base::server now can be controlled by derived class(echo_socket),
		//which is actually i_echo_server, so, we can invoke i_echo_server::test virtual function.
		server.test();
		st_server_socket_base::on_recv_error(ec);
	}

	//msg handling: send the original msg back(echo server)
	//congestion control, method #1, the peer needs its own congestion control too.
#ifndef ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER
	//this virtual function doesn't exists if ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER been defined
	virtual bool on_msg(out_msg_type& msg)
	{
		auto re = send_msg(msg.data(), msg.size());
		if (!re)
			congestion_control(true);
			//cannot handle (send it back) this msg timely, begin congestion control
			//'msg' will be put into receiving buffer, and be dispatched via on_msg_handle() in the future

		return re;
	}

	virtual bool on_msg_handle(out_msg_type& msg, bool link_down)
	{
		auto re = send_msg(msg.data(), msg.size());
		if (re)
			congestion_control(false);
			//successfully handled the only one msg in receiving buffer, end congestion control
			//subsequent msgs will be dispatched via on_msg() again.

		return re;
	}
#else
	//if we used receiving buffer, congestion control will become much simpler, like this:
	virtual bool on_msg_handle(out_msg_type& msg, bool link_down) {return send_msg(msg.data(), msg.size());}
#endif
	//msg handling end
};

class echo_server : public st_server_base<echo_socket, st_object_pool<echo_socket>, i_echo_server>
{
public:
	echo_server(st_service_pump& service_pump_) : st_server_base(service_pump_) {}

	statistic get_statistic()
	{
		statistic stat;
		do_something_to_all([&stat](object_ctype& item) {stat += item->get_statistic();});

		return stat;
	}

	//from i_echo_server, pure virtual function, we must implement it.
	virtual void test() {/*puts("in echo_server::test()");*/}
};

int main(int argc, const char* argv[])
{
	printf("usage: %s [<service thread number=1> [<port=%d> [ip=0.0.0.0]]]\n", argv[0], ST_ASIO_SERVER_PORT);
	puts("normal server's port will be 100 larger.");
	if (argc >= 2 && (0 == strcmp(argv[1], "--help") || 0 == strcmp(argv[1], "-h")))
		return 0;
	else
		puts("type " QUIT_COMMAND " to end.");

	st_service_pump sp;
	//only need a simple server? you can directly use st_server or st_server_base.
	//because we use st_server_socket_base directly, so this server cannot support fixed_length_unpacker and prefix_suffix_packer/prefix_suffix_unpacker,
	//the reason is these packer and unpacker need additional initializations that st_server_socket_base not implemented, see echo_socket's constructor for more details.
	typedef st_server_socket_base<packer, unpacker> normal_server_socket;
	st_server_base<normal_server_socket> server_(sp);
	echo_server echo_server_(sp); //echo server

	if (argc > 3)
	{
		server_.set_server_addr(atoi(argv[2]) + 100, argv[3]);
		echo_server_.set_server_addr(atoi(argv[2]), argv[3]);
	}
	else if (argc > 2)
	{
		server_.set_server_addr(atoi(argv[2]) + 100);
		echo_server_.set_server_addr(atoi(argv[2]));
	}
	else
		server_.set_server_addr(ST_ASIO_SERVER_PORT + 100);

	auto thread_num = 1;
	if (argc > 1)
		thread_num = std::min(16, std::max(thread_num, atoi(argv[1])));

#if 3 == PACKER_UNPACKER_TYPE
		global_packer->prefix_suffix("begin", "end");
#endif

	sp.start_service(thread_num);
	while(sp.is_running())
	{
		std::string str;
		std::cin >> str;
		if (QUIT_COMMAND == str)
			sp.stop_service();
		else if (RESTART_COMMAND == str)
		{
			sp.stop_service();
			sp.start_service(thread_num);
		}
		else if (LIST_STATUS == str)
		{
			printf("normal server, link #: " ST_ASIO_SF ", invalid links: " ST_ASIO_SF "\n", server_.size(), server_.invalid_object_size());
			printf("echo server, link #: " ST_ASIO_SF ", invalid links: " ST_ASIO_SF "\n", echo_server_.size(), echo_server_.invalid_object_size());
			puts("");
			puts(echo_server_.get_statistic().to_string().data());
		}
		//the following two commands demonstrate how to suspend msg dispatching, no matter recv buffer been used or not
		else if (SUSPEND_COMMAND == str)
			echo_server_.do_something_to_all([](echo_server::object_ctype& item) {item->suspend_dispatch_msg(true);});
		else if (RESUME_COMMAND == str)
			echo_server_.do_something_to_all([](echo_server::object_ctype& item) {item->suspend_dispatch_msg(false);});
		else if (LIST_ALL_CLIENT == str)
		{
			puts("clients from normal server:");
			server_.list_all_object();
			puts("clients from echo server:");
			echo_server_.list_all_object();
		}
		else
		{
			//broadcast series functions call pack_msg for each client respectively, because clients may used different protocols(so different type of packers, of course)
//			server_.broadcast_msg(str.data(), str.size() + 1);
			//send \0 character too, because asio_client used basic_buffer as its msg type, it will not append \0 character automatically as std::string does,
			//so need \0 character when printing it.

			//if all clients used the same protocol, we can pack msg one time, and send it repeatedly like this:
			packer p;
			auto msg = p.pack_msg(str.data(), str.size() + 1);
			//send \0 character too, because asio_client used basic_buffer as its msg type, it will not append \0 character automatically as std::string does,
			//so need \0 character when printing it.
			if (!msg.empty())
				server_.do_something_to_all([&msg](st_server_base<normal_server_socket>::object_ctype& item) {item->direct_send_msg(msg);});

			//if asio_client is using stream_unpacker, we don't need to pack message, just send it.
//			if (!str.empty())
//				server_.do_something_to_all([&str](st_server_base<normal_server_socket>::object_ctype& item) {item->direct_send_msg(str);});
		}
	}

	return 0;
}
