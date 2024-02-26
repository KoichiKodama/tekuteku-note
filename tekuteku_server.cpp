#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
#include <chrono>
#include <filesystem>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#ifdef USE_SSL
	#include <boost/beast/ssl.hpp>
	#include <boost/asio/ssl/context.hpp>
	#include <boost/beast/websocket/ssl.hpp>
#endif
#include <boost/format.hpp>
#include <json.hpp>
#include <k_time.hpp>
#include <tray.hpp>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"iphlpapi.lib")

#ifndef USE_SSL
static int DEFAULT_PORT = 80;
#else
static int DEFAULT_PORT = 443;
#endif

static std::string m_version = "build 2024-02-14";
static std::string m_server_name = "tekuteku-server";
static std::string m_logfile = "tekuteku-server.log";
static std::mutex m_mutex_log;
static int session_timeout = 21600;
static int debug_async_accept = 0;
static int debug_async_read = 0;
static int debug_async_write = 0;
static int debug_async_write_full = 0;
static int debug_whiteboard_update = 0;

void truncate_log() {
	if ( std::filesystem::exists(m_logfile) == false ) return;
	std::string fname_old = m_logfile+".old";
	if ( std::filesystem::copy_file(m_logfile,fname_old,std::filesystem::copy_options::overwrite_existing) == false ) return;
	std::ifstream ifs(fname_old);
	std::ofstream ofs(m_logfile,std::ios_base::trunc);
	if (!ifs) return;
	if (!ofs) return;
	std::string date_min = k_date_time(7);	// 7日以前のログを削除する
	std::string s;
	while ( std::getline(ifs,s) ) {
		if ( s.compare(0,date_min.length(),date_min) > 0 ) ofs << s << "\n";
	}
	ifs.close();
	std::filesystem::remove(fname_old);
}

bool log( const std::string& message, bool truncate = false ) {
	std::lock_guard<std::mutex> lock(m_mutex_log);
	std::ios_base::openmode mode = ( truncate ? std::ios_base::trunc : std::ios_base::app );
	std::ofstream out(m_logfile,mode);
	if (!out) return false;
	out << k_date_time() << " " << message;
	return true;
}

bool spawn_client( const std::string& host_url ) {
	return ( reinterpret_cast<uint64_t>(ShellExecute(NULL,"open",host_url.c_str(),NULL,NULL,SW_SHOWNORMAL)) > 32 ? true : false );
}

struct address_ipv4_t {
	address_ipv4_t() : c(0){};
	address_ipv4_t( uint32_t c ) : c(c) {};
	address_ipv4_t( int a0, int a1, int a2, int a3 ) {
		b[0] = (uint8_t)a0;
		b[1] = (uint8_t)a1;
		b[2] = (uint8_t)a2;
		b[3] = (uint8_t)a3;
	};
	uint32_t get( int i ) const { return (uint32_t)b[i]; };
	std::string to_string() const { return ( boost::format("%u.%u.%u.%u") % get(0) % get(1) % get(2) % get(3) ).str(); };
	union {
		uint32_t c;
		uint8_t b[4];
	};
	inline bool operator==( const address_ipv4_t& r ) const { return c == r.c; };
	inline bool operator!=( const address_ipv4_t& r ) const { return !( *this == r ); }
};

struct network_t {
	address_ipv4_t address;
	address_ipv4_t mask;
	address_ipv4_t broadcast;
};

bool enum_network( std::vector<network_t>& result ) {
	result.clear();
	PMIB_IPADDRTABLE pTable = NULL;
	DWORD dwSize = 0;
	if ( GetIpAddrTable(pTable,&dwSize,FALSE) != ERROR_INSUFFICIENT_BUFFER ) return false;
	pTable = (PMIB_IPADDRTABLE)malloc(dwSize);
	if ( GetIpAddrTable(pTable,&dwSize,FALSE) != NO_ERROR ) return false;
	address_ipv4_t loopback(127,0,0,1);
	for (auto i=0;i<pTable->dwNumEntries;i++) {
		MIB_IPADDRROW& e = pTable->table[i];
		if ( e.dwAddr == loopback.c ) continue;
		if ( e.wType & MIB_IPADDR_DISCONNECTED ) continue;
		network_t net;
		net.address.c = e.dwAddr;
		net.mask.c = e.dwMask;
		net.broadcast.c = (net.address.c|~(net.mask.c));//	GetIpAddrTable は正しい broadcast を戻さない。
		result.push_back(net);
	}
	free(pTable);
	return true;
}

using socket_t = boost::asio::ip::tcp::socket;
#ifdef USE_SSL
	using tcp_stream_t = boost::beast::ssl_stream<boost::beast::tcp_stream>;
	using websocket_stream_t = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
#else
	using tcp_stream_t = boost::beast::tcp_stream;
	using websocket_stream_t = boost::beast::websocket::stream<boost::beast::tcp_stream>;
#endif

struct taker_info_t {
	taker_info_t() : is_readonly(false),is_init(false) {};
	int num;
	std::string id;
	std::string text; // 未確定テキスト
	bool is_readonly;
	bool is_init;
};
struct whiteboard_element_t {
	whiteboard_element_t() : id(-1),edit(0),tobe_sent(true) {};
	whiteboard_element_t( const std::string& text, int id ) : text(text),id(id),edit(0),tobe_sent(true) {};
	std::string text;
	int id;
	int edit;
	bool tobe_sent;
};
std::string m_host_url;
std::map<std::shared_ptr<websocket_stream_t>,taker_info_t> m_takers;
std::vector<whiteboard_element_t> m_whiteboard;
boost::asio::ip::port_type m_port = DEFAULT_PORT;
std::vector<network_t> m_servers;
std::mutex m_mutex;
int num_connected = 0;	// 延べ接続テイカー
HANDLE request_broadcast;
bool stop_broadcast = false;
bool network_changed = false;
bool whiteboard_updated = false;
int whiteboard_voice_index = 0;

boost::beast::flat_buffer copy_to_buffer( const std::string& s ) {
	boost::beast::flat_buffer b;
	size_t n = boost::asio::buffer_copy(b.prepare(s.size()),boost::asio::buffer(s));
	b.commit(n);
	return b;
}

void broadcast_status( boost::asio::yield_context yield ) {
	nlohmann::json j_whiteboard = nlohmann::json::array();
	nlohmann::json j_whiteboard_full = nlohmann::json::array();

	while (true) {
//		boost::asio::post(boost::asio::get_associated_executor(yield),yield);
		if ( WaitForSingleObject(request_broadcast,INFINITE) != WAIT_OBJECT_0 || stop_broadcast ) break;

		std::map<std::shared_ptr<websocket_stream_t>,nlohmann::json> r_json;
		nlohmann::json j_takers;
		{
			std::lock_guard<std::mutex> lock(m_mutex);

			if ( whiteboard_updated == true ) {
				j_whiteboard.clear();
				j_whiteboard_full.clear();
				for (int i=0;i<m_whiteboard.size();i++) {
					auto& c = m_whiteboard[i];
					nlohmann::json x;
					x["text"] = c.text;
					x["i"] = i;
					x["edit"] = c.edit;
					j_whiteboard_full.push_back(x);
					if ( c.tobe_sent == true ) {
						j_whiteboard.push_back(x);
						c.tobe_sent = false;
					}
				}
				whiteboard_updated = false; debug_whiteboard_update++;
			}

			for (auto& e : m_takers) {
				const taker_info_t& t = e.second;
				nlohmann::json x;
				if (!t.is_readonly) x["text"] = t.text;
				x["id"] = t.id;
				x["num"] = t.num;
				j_takers.push_back(x);
			}

			for (auto& e : m_takers) {
				std::shared_ptr<websocket_stream_t> p_ws = e.first;
				taker_info_t& info = e.second;
				nlohmann::json x;
				x["type"] = 1;
				x["takers"] = j_takers;
				x["whiteboard_size"] = m_whiteboard.size();
				if ( info.is_init == true ) {
					x["whiteboard"] = j_whiteboard_full;
					info.is_init = false; debug_async_write_full++;
				}
				else x["whiteboard"] = j_whiteboard;
				r_json[p_ws] = x;
			}
		}

		for (auto& e : r_json) {
			std::shared_ptr<websocket_stream_t> p_ws = e.first;
			nlohmann::json& r = e.second;
			boost::beast::flat_buffer b = copy_to_buffer(r.dump());
			boost::system::error_code ec;
			if (( p_ws )&&( p_ws->is_open() == true )) {
				p_ws->async_write(b.data(),yield[ec]); debug_async_write++;
				if (ec) log((boost::format("websocket write error %s total=%d (%s)\n") % r["id"] % r_json.size() % ec.message()).str());
			}
			else log((boost::format("websocket write close %s total=%d\n") % r["id"] % r_json.size()).str());
		}

		if ( network_changed == true ) {
			network_changed = false;
			bool rc = false;
			std::string x = "127.0.0.1";
			for (auto ii : m_takers) {
				taker_info_t& info = ii.second;
				if ( info.id.compare(0,x.size(),x) == 0 ) {
					std::shared_ptr<websocket_stream_t> p_ws = ii.first;
					nlohmann::json r;
					r["type"] = 0;
					r["id"] = info.id;
					r["num"] = info.num;
					r["server"] = nlohmann::json::array();
					for (auto ii=m_servers.begin();ii!=m_servers.end();ii++) {
						const std::string a = (*ii).address.to_string();
						r["server"].push_back(( m_port == DEFAULT_PORT ? a : ( boost::format("%s:%d") % a % m_port ).str() ));
					}
					boost::beast::flat_buffer b = copy_to_buffer(r.dump());
					boost::system::error_code ec;
					p_ws->async_write(b.data(),yield[ec]); debug_async_write++;
					if (ec) {
						log((boost::format("websocket write error %s total=%d (%s)\n") % info.id % m_takers.size() % ec.message()).str());
					}
					else rc = true;
				}
			}
			#ifndef USE_SSL
			if ( rc == false ) { if ( spawn_client(m_host_url) == false ) log("error spawn_client\n"); }
			#endif
		}
	}
}

void exec_websocket_session( std::shared_ptr<websocket_stream_t> p_ws, boost::beast::http::request<boost::beast::http::string_body> req, boost::asio::yield_context yield ) {
	try {
		boost::system::error_code ec;
		p_ws->async_accept(req,yield); debug_async_accept++;

		auto ep = boost::beast::get_lowest_layer(*p_ws).socket().remote_endpoint(ec);
		taker_info_t info;
		info.id = ( boost::format("%s:%d") % ep.address().to_string() % ep.port() ).str();
		info.num = num_connected++;

		nlohmann::json r;
		r["type"] = 0;
		r["id"] = info.id;
		r["num"] = info.num;
		r["server"] = nlohmann::json::array();
		for (auto ii=m_servers.begin();ii!=m_servers.end();ii++) {
			const std::string a = (*ii).address.to_string();
			r["server"].push_back(( m_port == DEFAULT_PORT ? a : ( boost::format("%s:%d") % a % m_port ).str() ));
		}
		boost::beast::flat_buffer b = copy_to_buffer(r.dump());
		p_ws->async_write(b.data(),yield[ec]); debug_async_write++;	// m_takers 未登録なので broadcast_status とは干渉しない。
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_takers[p_ws] = info;
		}
		SetEvent(request_broadcast);
		log((boost::format("websocket session start %s total=%d\n") % info.id % m_takers.size()).str());

		for (;;) {
			taker_info_t& info = m_takers[p_ws];

			boost::beast::flat_buffer buffer;
			p_ws->text(true);
			p_ws->async_read(buffer,yield[ec]); debug_async_read++;
			if ( ec == boost::beast::websocket::error::closed ) {
				log((boost::format("websocket read close %s total=%d\n") % info.id % (m_takers.size()-1)).str());
				break;
			}
			if (ec) {
				log((boost::format("websocket read error %s total=%d (%s)\n") % info.id % (m_takers.size()-1) % ec.message()).str());
				break;
			}
			std::string s = boost::beast::buffers_to_string(buffer.data());
			if ( s.empty() == false ) {
				nlohmann::json json_i = nlohmann::json::parse(s);
				int status = json_i["status"];
				std::lock_guard<std::mutex> lock(m_mutex);
				if ( status == 9 ) info.is_init = true;
				if ( status == 2 ) {
					// 新規音声認識セッション開始
					std::for_each(m_whiteboard.begin()+whiteboard_voice_index,m_whiteboard.end(),[]( auto& c ){ c.id = -1; c.edit = 0; });
					whiteboard_voice_index = m_whiteboard.size();
				}
				if ( json_i.contains("text") == true ) {
					std::string text = json_i["text"];
					int text_index = json_i["text_index"];
					info.is_readonly = false;
					info.text = "";
					if ( status == 1 && text.empty() == false ) {	// 空文字は無視する仕様
						if ( text_index == -1 ) {
							m_whiteboard.push_back(whiteboard_element_t(text,-1));
						}
						else if ( text_index >= 0 && text_index < m_whiteboard.size() ) {
							auto& c = m_whiteboard[text_index];
							c.text = text;
							c.edit = 2;
							c.tobe_sent = true;
						}
						whiteboard_updated = true;
					}
					else {
						info.text = text;
						if ( text_index >= 0 && text_index < m_whiteboard.size() ) {
							auto& c = m_whiteboard[text_index];
							c.text = text;
							c.edit = 1;
							c.tobe_sent = true;
							whiteboard_updated = true;
						}
					}
				}
				else info.is_readonly = true;
				if ( json_i.contains("voice_text") == true && json_i["voice_text"].empty() == false ) {
					auto& l = json_i["voice_text"];
					for (const auto& x : l) {
						auto ii = std::find_if(m_whiteboard.begin()+whiteboard_voice_index,m_whiteboard.end(),[x]( const auto& c ){ return ( c.id == x["id"] ? true : false ); });
						if ( ii == m_whiteboard.end() ) {
							m_whiteboard.push_back(whiteboard_element_t(x["text"],x["id"]));
						}
						else {
							auto& c = (*ii);
							if ( c.edit == 0 ) {
								c.id = x["id"];
								c.text = x["text"];
								c.tobe_sent = true;
							}
						}
					}
					int id_max = l[l.size()-1]["id"];
					bool is_erased = false;
					for (auto ii=m_whiteboard.begin()+whiteboard_voice_index;ii!=m_whiteboard.end();) {
						if ( (*ii).id > id_max ) {
							ii = m_whiteboard.erase(ii);
							is_erased = true;
						}
						else {
							if (is_erased) (*ii).tobe_sent = true;
							ii++;
						}
					}
					whiteboard_updated = true;
				}
				SetEvent(request_broadcast);
			}
		}
		log((boost::format("websocket session stop %s total=%d\n") % info.id % m_takers.size()).str());
	}
	catch ( boost::system::system_error& e ) {
		log((boost::format("boost exception in exec_websocket_session %s : %s\n") % m_takers[p_ws].id % e.what()).str());
	}
	catch ( std::exception& e ) {
		log((boost::format("exception in exec_websocket_session %s : %s\n") % m_takers[p_ws].id % e.what()).str());
	}
	if ( m_takers.find(p_ws) != m_takers.end() ) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_takers.erase(p_ws);
	}
	SetEvent(request_broadcast);
}


boost::beast::string_view mime_type( boost::beast::string_view path ) {
	auto const ext = [&path]{
		auto const pos = path.rfind(".");
		if ( pos == boost::beast::string_view::npos ) return boost::beast::string_view{};
		return path.substr(pos);
	}();
	if ( boost::beast::iequals(ext,".html")) return "text/html";
	if ( boost::beast::iequals(ext,".css" )) return "text/css";
	if ( boost::beast::iequals(ext,".txt" )) return "text/plain";
	if ( boost::beast::iequals(ext,".js"  )) return "application/javascript";
	if ( boost::beast::iequals(ext,".json")) return "application/json";
	if ( boost::beast::iequals(ext,".png" )) return "image/png";
	if ( boost::beast::iequals(ext,".jpg" )) return "image/jpeg";
	if ( boost::beast::iequals(ext,".gif" )) return "image/gif";
	if ( boost::beast::iequals(ext,".bmp" )) return "image/bmp";
	if ( boost::beast::iequals(ext,".ico" )) return "image/vnd.microsoft.icon";
	if ( boost::beast::iequals(ext,".svg" )) return "image/svg+xml";
	return "application/text";
}

std::string path_cat( boost::beast::string_view base, boost::beast::string_view path ) {
	if (base.empty()) return std::string(path);
	std::string result(base);
	char constexpr path_separator = '\\';
	if ( result.back() == path_separator ) result.resize(result.size()-1);
	result.append(path.data(),path.size());
	for ( auto& c : result ) {
		if ( c == '/' ) c = path_separator;
	}
	return result;
}

struct send_lambda {
	tcp_stream_t& stream_;
	bool& close_;
	boost::beast::error_code& ec_;
	boost::asio::yield_context yield_;

	send_lambda( tcp_stream_t& stream, bool& close, boost::system::error_code& ec, boost::asio::yield_context yield ) : stream_(stream),close_(close),ec_(ec),yield_(yield) {}
	template<bool isRequest,class Body,class Fields> void operator()( boost::beast::http::message<isRequest,Body,Fields>&& msg ) const {
		// Determine if we should close the connection after
		close_ = msg.need_eof();
		boost::beast::http::serializer<isRequest,Body,Fields> sr{msg};
		boost::beast::http::async_write(stream_,sr,yield_[ec_]);
	}
};

void exec_http_session( tcp_stream_t& stream, boost::asio::yield_context yield ) {
	boost::beast::error_code ec;
	boost::asio::ip::tcp::socket& socket = boost::beast::get_lowest_layer(stream).socket();
	boost::asio::ip::tcp::socket::endpoint_type ep = socket.remote_endpoint(ec);
	boost::beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(session_timeout));
	#ifdef USE_SSL
    stream.async_handshake(boost::asio::ssl::stream_base::server,yield[ec]);
	#endif
	boost::beast::flat_buffer buffer;
	boost::beast::http::request<boost::beast::http::string_body> req;

	boost::beast::http::async_read(stream,buffer,req,yield[ec]);
	if ( ec == boost::beast::http::error::end_of_stream ) {
		socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send,ec);
		return;
	}
	if (ec) return;
	if ( boost::beast::websocket::is_upgrade(req) ) {
		auto p_ws = std::make_shared<websocket_stream_t>(std::move(stream));
		boost::asio::spawn(boost::beast::get_lowest_layer(*p_ws).socket().get_executor(),std::bind(&exec_websocket_session,p_ws,req,std::placeholders::_1));
		return;
	}

	bool close = false;
	send_lambda send{stream,close,ec,yield};

	auto const bad_request = [&req](boost::beast::string_view why){
		boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::bad_request,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,"text/html");
		res.keep_alive(req.keep_alive());
		res.body() = std::string(why);
		res.prepare_payload();
		return res;
	};

	// Returns a not found response
	auto const not_found = [&req](boost::beast::string_view target){
		boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::not_found,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,"text/html");
		res.keep_alive(req.keep_alive());
		res.body() = "The resource '" + std::string(target) + "' was not found.";
		res.prepare_payload();
		return res;
	};

	// Returns a server error response
	auto const server_error = [&req](boost::beast::string_view what){
		boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::internal_server_error,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,"text/html");
		res.keep_alive(req.keep_alive());
		res.body() = "An error occurred: '" + std::string(what) + "'";
		res.prepare_payload();
		return res;
	};

	if ( req.method() != boost::beast::http::verb::get && req.method() != boost::beast::http::verb::head ) return send(bad_request("Unknown HTTP-method"));
	if ( req.target().empty() || req.target()[0] != '/' || req.target().find("..") != boost::beast::string_view::npos ) return send(bad_request("Illegal request-target"));

	std::string path = path_cat(".\\",req.target());
	if ( req.target().back() == '/' ) path.append("index.html");

	boost::beast::http::file_body::value_type body;
	body.open(path.c_str(),boost::beast::file_mode::scan,ec);
	if ( ec == boost::system::errc::no_such_file_or_directory ) return send(not_found(req.target()));
	if (ec) return send(server_error(ec.message()));

	auto const size = body.size();
	if ( req.method() == boost::beast::http::verb::head ) {
		boost::beast::http::response<boost::beast::http::empty_body> res{boost::beast::http::status::ok,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,mime_type(path));
		res.content_length(size);
		res.keep_alive(req.keep_alive());
		return send(std::move(res));
	}
	// GET
	boost::beast::http::response<boost::beast::http::file_body> res{std::piecewise_construct,std::make_tuple(std::move(body)),std::make_tuple(boost::beast::http::status::ok,req.version())};
	res.set(boost::beast::http::field::server,m_server_name);
	res.set(boost::beast::http::field::content_type,mime_type(path));
	res.content_length(size);
	res.keep_alive(req.keep_alive());
	return send(std::move(res));
}

#ifdef USE_SSL
void exec_listen( boost::asio::io_context& ioc, boost::asio::ssl::context& ctx, boost::asio::ip::tcp::endpoint endpoint, boost::asio::yield_context yield ) {
	boost::asio::ip::tcp::acceptor acceptor(ioc);
	acceptor.open(endpoint.protocol());
	acceptor.set_option(boost::asio::socket_base::reuse_address(true));
	acceptor.bind(endpoint);
	acceptor.listen(boost::asio::socket_base::max_listen_connections);
	for (;;) {
		socket_t socket(ioc);
		boost::system::error_code ec;
		acceptor.async_accept(socket,yield[ec]);
		if (!ec) { boost::asio::spawn(ioc,std::bind(&exec_http_session,tcp_stream_t(std::move(socket),ctx),std::placeholders::_1),boost::asio::detached); }
	}
}
#else
void exec_listen( boost::asio::io_context& ioc, boost::asio::ip::tcp::endpoint endpoint, boost::asio::yield_context yield ) {
	boost::asio::ip::tcp::acceptor acceptor(ioc);
	acceptor.open(endpoint.protocol());
	acceptor.set_option(boost::asio::socket_base::reuse_address(true));
	acceptor.bind(endpoint);
	acceptor.listen(boost::asio::socket_base::max_listen_connections);
	for (;;) {
		socket_t socket(ioc);
		boost::system::error_code ec;
		acceptor.async_accept(socket,yield[ec]);
		if (!ec) { boost::asio::spawn(ioc,std::bind(&exec_http_session,tcp_stream_t(std::move(socket)),std::placeholders::_1)); }
	}
}
#endif

class network_watchdog {
public:
	network_watchdog() {
		is_stopped = false;
		h = NULL;
		o.hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
		start();
	};
	~network_watchdog() { CancelIPChangeNotify(&o); CloseHandle(o.hEvent); };
	bool start() { return ( NotifyAddrChange(&h,&o) == ERROR_IO_PENDING ); };
	void stop() { is_stopped = true; SetEvent(o.hEvent); };
	bool wait() { DWORD rc = WaitForSingleObject(o.hEvent,INFINITE); return ( rc == WAIT_OBJECT_0 && is_stopped == false ? true : false ); }
private:
	HANDLE h;
	OVERLAPPED o;
	bool is_stopped;
};

network_watchdog wd; static std::thread thread_wd{};
boost::asio::io_context ioc_w(1); static std::thread thread_w{};
boost::asio::io_context ioc_r(1); static std::thread thread_r{};
bool save_whiteboard = true;

void terminate_server() {
	stop_broadcast = true; SetEvent(request_broadcast);
	wd.stop(); thread_wd.join();
	ioc_r.stop(); thread_r.join();
	ioc_w.stop(); thread_w.join();
	if ( save_whiteboard == true ) {
		log("==== whiteboard ====\n");
		std::for_each(m_whiteboard.begin(),m_whiteboard.end(),[]( const auto& c ){ log(c.text+"\n"); });
		log("==== whiteboard ====\n");
	}
	log((boost::format("debug_async_accept = %d\n") % debug_async_accept).str());
	log((boost::format("debug_async_read = %d\n") % debug_async_read).str());
	log((boost::format("debug_async_write = %d\n") % debug_async_write).str());
	log((boost::format("debug_async_write_full = %d\n") % debug_async_write_full).str());
	log((boost::format("debug_whiteboard_update = %d\n") % debug_whiteboard_update).str());
	log("service stopped\n");
}

#ifdef USE_SSL
static boost::asio::ssl::context ctx{boost::asio::ssl::context::tlsv12};
void load_server_certificate( boost::asio::ssl::context& ctx ) {
	// 証明書
	// m_server_name の値は無関係。
	// 証明書記載の common name でアクセスしないと NET::ERR_CERT_COMMON_NAME_INVALID となるので localhost で開いてはダメ。
	// nii-odca4g7rsa.cer を emulsion-labo.physics.aichi-edu.ac.jp.cer の末尾にコピーすれば動作する。

	ctx.set_password_callback([](std::size_t,boost::asio::ssl::context_base::password_purpose) { return ""; });
	ctx.use_private_key_file("./tekuteku-server.key",boost::asio::ssl::context::file_format::pem);
	ctx.use_certificate_chain_file("./tekuteku-server.cer");
	ctx.set_options( 
		boost::asio::ssl::context::default_workarounds |
		boost::asio::ssl::context::no_sslv2 );
}
#endif

int main( int argc, char** argv ) {
	try {
		bool save_whiteboard = true;

		argc--; argv++;
		while ( argc != 0 ) {
			if ( strcmp(*argv,"--port") == 0 ) {
				argc--; argv++;
				m_port = atoi(*argv);
				m_logfile = ( boost::format("tekuteku-server-%04d.log") % m_port ).str();
				log(( boost::format("option port=%d\n") % m_port ).str());
			}
			argc--; argv++;
		}

		//
		// 同一ポートでの多重起動禁止はトレーの存在確認で行う。
		//
		std::string tray_name = (boost::format("tekuteku-%04d") % m_port).str().c_str();
		if ( tray_exist(tray_name.c_str()) == true ) {
			log("stop due to multiple servers\n");
			MessageBoxW(NULL,L"同じポートでは、複数のサーバを動かせません。",L"てくてくノートサーバ",MB_OK);
			return 0;
		}

		#ifdef USE_SSL
		load_server_certificate(ctx);
		#endif

		truncate_log();
		log((boost::format("started %s\n") % m_version).str());
		if ( enum_network(m_servers) == false ) throw std::runtime_error("enum_network");
		if ( m_servers.empty() ) { log("no network\n"); return 0; }
		std::for_each(m_servers.begin(),m_servers.end(),[](const network_t& net){ log((boost::format("server : %s / %s\n") % net.address.to_string() % net.mask.to_string()).str()); });
		#ifndef USE_SSL
		m_host_url = (boost::format("http://localhost:%d") % m_port).str();	// Chrome でマイク使用ブロックを解除できないので localhost を使用する。
		if ( !spawn_client(m_host_url) ) throw std::runtime_error("spawn_client");
		#endif

		thread_r = std::move(std::thread([]{
			auto const ep = boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("0.0.0.0"),m_port};
			#ifndef USE_SSL
			boost::asio::spawn(ioc_r,std::bind(&exec_listen,std::ref(ioc_r),ep,std::placeholders::_1));
			#else
			boost::asio::spawn(ioc_r,std::bind(&exec_listen,std::ref(ioc_r),std::ref(ctx),ep,std::placeholders::_1));
			#endif
			ioc_r.run();
			m_takers.clear();
		}));

		request_broadcast = CreateEvent(NULL,FALSE,FALSE,NULL);
		thread_w = std::move(std::thread([]{
			boost::asio::spawn(ioc_w,std::bind(&broadcast_status,std::placeholders::_1));
			ioc_w.run();
		}));

		thread_wd = std::move(std::thread([]{
			while ( wd.wait() ) {
				std::vector<network_t> l;
				if ( enum_network(l) == false ) throw std::runtime_error("enum_network");
				if ( std::equal(l.begin(),l.end(),m_servers.begin(),m_servers.end(),[]( const network_t& a, const network_t& b ){ return a.address == b.address; }) == false ) {
					m_servers.swap(l);
					log("network changed\n");
					if ( m_servers.empty() ) log("no network\n");
					std::for_each(m_servers.begin(),m_servers.end(),[](const network_t& net){ log((boost::format("server : %s / %s\n") % net.address.to_string() % net.mask.to_string()).str()); });
					network_changed = true;
					SetEvent(request_broadcast);
				}
				wd.start();
			}
		}));

		if ( tray_init(tray_t("tekuteku-icon.png",terminate_server),(boost::format("tekuteku-%04d") % m_port).str().c_str()) != 0 ) {
			log("failed to start tray\n");
			terminate_server();
			return 0;
		}
		while ( tray_loop(1) == 0 ) {}
		CloseHandle(request_broadcast);
		return 0;
	}
	catch ( boost::system::system_error& e ) { log((boost::format("boost exception : %s\n") % e.what()).str()); }
	catch ( std::exception& e ) { log((boost::format("exception : %s\n") % e.what()).str()); }
	catch ( ... ) { log("unknown exception\n"); }
	MessageBoxW(NULL,L"エラーのため終了します。もう一度動かして下さい。",L"てくてくノートサーバ",MB_OK);
	return 1;
}
