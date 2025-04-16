#ifdef _WINDOWS
	#include <SDKDDKVer.h>
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <shellapi.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <iphlpapi.h>
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <ifaddrs.h>
#endif
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
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
#include <boost/process.hpp>

#include <json.hpp>
#include <k_time.hpp>
#include <tray.hpp>

#ifdef _WINDOWS
	#pragma comment(lib,"user32.lib")
	#pragma comment(lib,"shell32.lib")
	#pragma comment(lib,"ws2_32.lib")
	#pragma comment(lib,"iphlpapi.lib")
#endif

#ifndef USE_SSL
static int DEFAULT_PORT = 80;
#else
static int DEFAULT_PORT = 443;
#endif

static nlohmann::json m_cfg;
static std::string m_version = "build 2025-04-11";
static std::string m_server_name = "tekuteku-server";
static std::string m_magic;
static std::string m_logfile = "tekuteku-server.log";
static std::mutex m_mutex_log;
static int session_timeout = 21600;
static int debug_async_accept = 0;
static int debug_async_read = 0;
static int debug_write = 0;
static int debug_write_full = 0;
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
	ofs.close();
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
	#ifdef _WINDOWS
		return ( reinterpret_cast<uint64_t>(ShellExecute(NULL,"open",host_url.c_str(),NULL,NULL,SW_SHOWNORMAL)) > 32 ? true : false );
	#else
		return true;
	#endif
}

struct address_ipv4_t {
	address_ipv4_t() : c(0){};
	address_ipv4_t( uint32_t c, const std::string& byte_order = "net" ) : c(c) {
		if ( byte_order == "host" ) {
			union { uint32_t c; uint8_t b[4]; } x;
			x.c = c;
			b[0] = x.b[3];
			b[1] = x.b[2];
			b[2] = x.b[1];
			b[3] = x.b[0];
		}
	};
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
	address_ipv4_t broadcast; // = (net.address.c|~(net.mask.c))
};

bool enum_network( std::vector<network_t>& result ) {
	result.clear();
	#ifdef _WINDOWS
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
	#else
	struct ifaddrs *ifa,*i;
	if ( getifaddrs(&ifa) == 0 ) {
		address_ipv4_t loopback(127,0,0,1);
		for ( i=ifa;i!=NULL;i=i->ifa_next) {
			if ( i->ifa_addr == NULL ) continue;
			if ( i->ifa_addr->sa_family != AF_INET ) continue; // AF_PACKET,AF_INET, AF_INET6
			struct sockaddr_in* addr = (struct sockaddr_in*)(i->ifa_addr);
			struct sockaddr_in* mask = (struct sockaddr_in*)(i->ifa_netmask);
			if ( addr->sin_addr.s_addr == loopback.c ) continue;
			network_t net;
			net.address.c = addr->sin_addr.s_addr;
			net.mask.c = mask->sin_addr.s_addr;
			net.broadcast.c = (net.address.c|~(net.mask.c));//	GetIpAddrTable は正しい broadcast を戻さない。
			result.push_back(net);
		}
		freeifaddrs(ifa);
	}
	#endif
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
	taker_info_t() : is_readonly(false),is_init(false),whiteboard_voice_index(0) {};
	int num;
	std::string id;
	std::string text; // 未確定テキスト
	bool is_readonly;
	bool is_init;
	int whiteboard_voice_index;
};
struct whiteboard_element_t {
	whiteboard_element_t() : id(-1),edit(0),tobe_sent(true),num(-1) {};
	whiteboard_element_t( const std::string& text, int id, int num ) : text(text),id(id),edit(0),tobe_sent(true),num(num) {};
	std::string text;
	int num;	// taker の通番
	int id;		// taker 毎の音声認識の行番号
	int edit;
	bool tobe_sent;
};

std::map<std::shared_ptr<websocket_stream_t>,taker_info_t> m_takers;
std::vector<whiteboard_element_t> m_whiteboard;
boost::asio::ip::port_type m_port = DEFAULT_PORT;
std::vector<network_t> m_servers;
std::mutex m_mutex;
int num_connected = 0;	// 延べ接続テイカー
bool network_changed = false;
bool whiteboard_updated = false;

std::vector<std::string> split( const std::string& x ) {
	std::vector<std::string> l;
	std::stringstream ss(x);
	std::string e;
	while ( std::getline(ss,e,'\n') ) { if (!e.empty()) l.push_back(e); }
	return l;
}

void log_whiteboard() {
	log((boost::format("==== whiteboard (%d) ====\n") % m_whiteboard.size()).str());
	std::for_each(m_whiteboard.begin(),m_whiteboard.end(),[]( const auto& c ){
		const std::vector<std::string> l = split(c.text);
		std::for_each(l.begin(),l.end(),[]( const auto& s ){ log(s+"\n"); });
	});
	log("==== whiteboard ====\n");
}

boost::beast::flat_buffer copy_to_buffer( const std::string& s ) {
	boost::beast::flat_buffer b;
	size_t n = boost::asio::buffer_copy(b.prepare(s.size()),boost::asio::buffer(s));
	b.commit(n);
	return b;
}

/*
class request_broadcast_event_t {
public:
	request_broadcast_event_t() : m_requested(false),m_stop(false) {};
	virtual ~request_broadcast_event_t() {};
	void set() {
		{
			std::lock_guard<std::mutex> lock(m_mtx);
			m_requested = true;
		}
		m_cv.notify_all();
	};
	void stop() { m_stop = true; set(); };
	bool wait() {
		std::unique_lock<std::mutex> lock(m_mtx);
		m_cv.wait(lock,[this]{ return m_requested; });
		m_requested = false;
		return ( m_stop == true ? false : true );
	};
	bool is_stopped() const { return m_stop; };
private:
	bool m_requested;
	bool m_stop;
	std::mutex m_mtx;
	std::condition_variable m_cv;
};
request_broadcast_event_t request_broadcast{};
*/

class request_broadcast_event_t {
public:
	request_broadcast_event_t( boost::asio::io_context& ioc ) : m_stop(false),m_requested(false),m_timer(ioc) {};
	virtual ~request_broadcast_event_t() {};
	void set() {
		{
			std::lock_guard<std::mutex> lock(m_mtx);
			m_requested = true;
		}
		m_timer.cancel();
	};
	void stop() { m_stop = true; set(); };
	bool wait( boost::asio::yield_context yield ) {
		boost::system::error_code ec;
		for (;;) {
			m_timer.expires_after(boost::asio::chrono::seconds(60));
			m_timer.async_wait(yield[ec]);
			std::lock_guard<std::mutex> lock(m_mtx);
			if ( m_requested == true ) { m_requested = false; break; }
		}
		return ( m_stop == true ? false : true );
	};
	bool is_stopped() const { return m_stop; };
private:
	bool m_requested;
	bool m_stop;
	std::mutex m_mtx;
	boost::asio::steady_timer m_timer;
};
boost::asio::io_context ioc_r(1); static std::thread thread_r{};
request_broadcast_event_t request_broadcast{ioc_r};

const std::string get_taker_id( const std::shared_ptr<websocket_stream_t>& p_ws ) {
	if ( m_takers.find(p_ws) == m_takers.end() ) return "erased-socket";
	return m_takers.at(p_ws).id;
}

void broadcast_status( boost::asio::yield_context yield ) {
	nlohmann::json j_whiteboard = nlohmann::json::array();
	nlohmann::json j_whiteboard_full = nlohmann::json::array();

	while (true) {
//		if ( request_broadcast.wait() == false ) break;
		if ( request_broadcast.wait(yield) == false ) break;

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
			else j_whiteboard.clear();

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
				taker_info_t& t = e.second;
				nlohmann::json x;
				x["type"] = 1;
				x["takers"] = j_takers;
				x["whiteboard_size"] = m_whiteboard.size();
				if ( t.is_init == true ) {
					x["whiteboard"] = j_whiteboard_full;
					t.is_init = false; debug_write_full++;
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
				p_ws->write(b.data(),ec); debug_write++; // boost\beast\core\detail\stream_base.hpp(116) assert in raspberry-pi4 対策
				if (ec) log((boost::format("write error %s (%s)\n") % get_taker_id(p_ws) % ec.message()).str());
			}
			else log((boost::format("write close %s\n") % get_taker_id(p_ws)).str());
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
					p_ws->write(b.data(),ec); debug_write++; // boost\beast\core\detail\stream_base.hpp(116) assert in raspberry-pi4 対策
					if (ec) {
						log((boost::format("write error %s (%s)\n") % info.id % ec.message()).str());
					}
					else rc = true;
				}
			}
		}
	}
}

void exec_websocket_session( std::shared_ptr<websocket_stream_t> p_ws, boost::beast::http::request<boost::beast::http::string_body> req, boost::asio::yield_context yield ) {
	try {
		boost::system::error_code ec;
		p_ws->async_accept(req,yield); debug_async_accept++;
//		p_ws->text(false);

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
		p_ws->write(b.data(),ec); debug_write++;	// m_takers 未登録なので broadcast_status とは干渉しない。
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_takers[p_ws] = info;
		}
		request_broadcast.set();
		log((boost::format("session start %s total=%d\n") % info.id % m_takers.size()).str());

		for (;;) {
			taker_info_t& info = m_takers[p_ws];

			boost::beast::flat_buffer buffer;
			p_ws->async_read(buffer,yield[ec]); debug_async_read++;
			if ( ec == boost::beast::websocket::error::closed ) {
				log((boost::format("read close %s\n") % info.id).str());
				break;
			}
			#ifdef USE_SSL
			if ( ec && ec != boost::asio::ssl::error::stream_truncated ) {
			#else
			if (ec) {
			#endif
				log((boost::format("read error %s (%s)\n") % info.id % ec.message()).str());
				break;
			}
			std::string s = boost::beast::buffers_to_string(buffer.data());
			if ( s.empty() == false ) {
				nlohmann::json json_i = nlohmann::json::parse(s);
				int status = json_i["status"];
				std::lock_guard<std::mutex> lock(m_mutex);
				if ( status == 8 ) {
					log((boost::format("clear whiteboard by %s\n") % info.id).str());
					log_whiteboard();
					m_whiteboard.clear();
					std::for_each(m_takers.begin(),m_takers.end(),[]( auto& e ){ e.second.whiteboard_voice_index = 0; });
					whiteboard_updated = true;
				}
				if ( status == 9 ) info.is_init = true;
				if ( status == 2 ) {
					// 新規音声認識セッション開始
					std::for_each(m_whiteboard.begin()+info.whiteboard_voice_index,m_whiteboard.end(),[&info]( auto& c ){ if ( info.num == c.num ) { c.id = -1; c.edit = 0; } });
					info.whiteboard_voice_index = m_whiteboard.size();
				}
				if ( json_i.contains("text") == true ) {
					std::string text = json_i["text"];
					int text_index = json_i["text_index"];
					info.is_readonly = false;
					info.text = "";
					if ( status == 1 && text.empty() == false ) {	// 空文字は無視する仕様
						if ( text_index == -1 ) {
							m_whiteboard.push_back(whiteboard_element_t(text,-1,info.num));
						}
						else if ( text_index >= 0 && text_index < m_whiteboard.size() ) {
							auto& c = m_whiteboard[text_index];
							c.text = text;
							c.edit = 2;
							c.tobe_sent = true;
						}
						else log((boost::format("unexpected text %s %d '%s'\n") % info.id % text_index % text).str());
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
						auto ii = std::find_if(m_whiteboard.begin()+info.whiteboard_voice_index,m_whiteboard.end(),[x,&info]( const auto& c ){ return ( c.num == info.num && c.id == x["id"] ? true : false ); });
						if ( ii == m_whiteboard.end() ) {
							m_whiteboard.push_back(whiteboard_element_t(x["text"],x["id"],info.num));
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
					for (auto ii=m_whiteboard.begin()+info.whiteboard_voice_index;ii!=m_whiteboard.end();) {
						if ( (*ii).num != info.num ) { ii++; continue; }
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
				request_broadcast.set();
			}
		}
		log((boost::format("session stop %s\n") % info.id).str());
	}
	catch ( boost::system::system_error& e ) { log((boost::format("boost exception in exec_websocket_session %s : %s\n") % get_taker_id(p_ws) % e.what()).str()); }
	catch ( std::exception& e ) { log((boost::format("exception in exec_websocket_session %s : %s\n") % get_taker_id(p_ws) % e.what()).str()); }
	if ( m_takers.find(p_ws) != m_takers.end() ) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_takers.erase(p_ws);
	}
	log((boost::format("session terminated total=%d\n") % m_takers.size()).str());
	request_broadcast.set();
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
	if ( boost::beast::iequals(ext,".py"  )) return "text/html";
	return "application/text";
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

class http_query_t {
public:
	http_query_t( std::string s ) { parse(s); };
	~http_query_t() {};
	void parse( std::string s ) {
		m_envs.clear();
		std::string::size_type ii = 0;
		std::string::size_type jj = s.find_first_of("&"); if ( jj == std::string::npos ) jj = s.size();
		while ( ii < s.size() ) {
			std::string a = s.substr(ii,jj-ii);
			std::string::size_type i = a.find("=");
			m_envs[a.substr(0,i)] = ( i == std::string::npos ? "" : a.substr(i+1) );
			ii = jj+1;
			jj = s.find_first_of("&",ii);
			if ( jj == std::string::npos ) jj = s.size();
		}
	};
	std::string args() const {
		std::string r;
		std::for_each(m_envs.begin(),m_envs.end(),[&r]( const auto& c ){ r += (" "+c.first+"="+c.second); });
		return r;
	};
	std::string env( const std::string& key ) const {
		auto ii = m_envs.find(key);
		return ( ii == m_envs.end() ? "" : (*ii).second );
	};

private:
	std::map<std::string,std::string> m_envs;
};

void exec_http_session( tcp_stream_t& stream, boost::asio::yield_context yield ) {
	boost::beast::error_code ec;
	boost::asio::ip::tcp::socket& socket = boost::beast::get_lowest_layer(stream).socket();
	boost::asio::ip::tcp::socket::endpoint_type ep = socket.remote_endpoint(ec);
//	boost::beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(session_timeout));
	#ifdef USE_SSL
    stream.async_handshake(boost::asio::ssl::stream_base::server,yield[ec]);
	#endif
	boost::beast::flat_buffer buffer;
	boost::beast::http::request<boost::beast::http::string_body> req;

	auto const error_responce = [&req]( boost::beast::http::status status, boost::beast::string_view why ){
		boost::beast::http::response<boost::beast::http::string_body> res{status,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,"text/html");
		res.keep_alive(req.keep_alive());
		res.body() = std::string(why);
		res.prepare_payload();
		return res;
	};

	bool close = false;
	send_lambda send{stream,close,ec,yield};

	boost::beast::http::async_read(stream,buffer,req,yield[ec]);
	if ( ec == boost::beast::http::error::end_of_stream ) {
		socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send,ec);
		return;
	}
	if (ec) return;
	if ( boost::beast::websocket::is_upgrade(req) ) {
		if (!m_magic.empty()) {
			std::string::size_type x = req.target().find("?");
			std::string query = ( x != std::string::npos ? req.target().substr(x) : "" );
			if ( query != ("?magic="+m_magic) ) return send(error_responce(boost::beast::http::status::bad_request,"authentication failure"));
		}
		auto p_ws = std::make_shared<websocket_stream_t>(std::move(stream));
		boost::asio::spawn(boost::beast::get_lowest_layer(*p_ws).socket().get_executor(),std::bind(&exec_websocket_session,p_ws,req,std::placeholders::_1));
		return;
	}

	if ( req.method() != boost::beast::http::verb::get && req.method() != boost::beast::http::verb::head ) return send(error_responce(boost::beast::http::status::bad_request,"Unknown HTTP-method"));
	if ( req.target().empty() || req.target()[0] != '/' || req.target().find("..") != boost::beast::string_view::npos ) return send(error_responce(boost::beast::http::status::bad_request,"Illegal request-target"));

	std::string target = req.target();
	std::string::size_type i_query = target.find("?");
	std::string::size_type i_anchor = target.find("#");
	std::string path = target.substr(0,i_query); if ( path.back() == '/' ) path += "index.html";
	std::string query = ( i_query == std::string::npos ? "" : target.substr(i_query+1,i_anchor) );
	std::string anchor = ( i_anchor == std::string::npos ? "" : target.substr(i_anchor+1) );
	std::string path_ex = ( path.find_last_of(".") == std::string::npos ? "" : path.substr(path.find_last_of(".")+1) );
	http_query_t http_query(query);
	if ( path == "/index.html" && !m_magic.empty() && http_query.env("magic") != m_magic ) {
		// magic 確認は index.html に限定
		return send(error_responce(boost::beast::http::status::bad_request,"authentication failure"));
	}

	boost::beast::http::response<boost::beast::http::empty_body> res{boost::beast::http::status::ok,req.version()};
	res.set(boost::beast::http::field::server,m_server_name);
	res.keep_alive(req.keep_alive());

	if ( path_ex == "py" || path_ex == "sh" ) {
		boost::beast::http::string_body::value_type body;
		boost::process::ipstream is;
		std::error_code ec;
		#ifdef _WINDOWS
		boost::process::child c("bash -c '."+path+http_query.args()+"'",boost::process::std_out>is,ec);
		#else
		boost::process::child c("."+path+http_query.args(),boost::process::std_out>is,ec);
		#endif
		c.wait();
		if (!ec) {
			bool end_of_header = false;
			std::string x;
			while ( is && std::getline(is,x) ) {
				if ( x.back() == '\r' ) x.pop_back(); // getline は改行コードを含まない。windows での \r 削除。
				if (end_of_header) body += (x+"\n");
				if (x.empty()) end_of_header = true;
			}
			auto const size = body.size();
		boost::process::child c("."+path+http_query.args(),boost::process::std_out>is,ec);
			res.set(boost::beast::http::field::content_type,"text/plain");
			res.content_length(size);
			if ( req.method() == boost::beast::http::verb::head ) return send(std::move(res));
			boost::beast::http::response<boost::beast::http::string_body> res_x{res};
			res_x.body() = std::move(body);
			return send(std::move(res_x));
		}
		else {
			std::string m = (boost::format("error (value,category,message) = (%d,%s,%s)") % ec.value() % ec.category().name() % ec.message() ).str();
			send(error_responce(boost::beast::http::status::internal_server_error,m)); // 500
		}
	}
	else {
		std::string file_path = "." + path;
		boost::beast::http::file_body::value_type body;
		body.open(file_path.c_str(),boost::beast::file_mode::scan,ec);
		if ( ec == boost::system::errc::no_such_file_or_directory ) return send(error_responce(boost::beast::http::status::not_found,req.target()));
		if (ec) return send(error_responce(boost::beast::http::status::internal_server_error,ec.message()));
		auto const size = body.size();
		res.set(boost::beast::http::field::content_type,mime_type(file_path));
		res.content_length(size);
		if ( req.method() == boost::beast::http::verb::head ) return send(std::move(res));
		boost::beast::http::response<boost::beast::http::file_body> res_x{res};
		res_x.body() = std::move(body);
		return send(std::move(res_x));
	}
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

#ifdef _WINDOWS
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
#endif

// boost::asio::io_context ioc_w(1); static std::thread thread_w{};
// boost::asio::io_context ioc_r(1); static std::thread thread_r{};

void terminate_server() {
	request_broadcast.stop();
//	ioc_w.stop(); thread_w.join();
	ioc_r.stop(); thread_r.join();
	if (true) log_whiteboard();
	log((boost::format("debug_async_accept = %d\n") % debug_async_accept).str());
	log((boost::format("debug_async_read = %d\n") % debug_async_read).str());
	log((boost::format("debug_write = %d\n") % debug_write).str());
	log((boost::format("debug_write_full = %d\n") % debug_write_full).str());
	log((boost::format("debug_whiteboard_update = %d\n") % debug_whiteboard_update).str());
	log("service stopped\n");
}

std::string load_file_all( const std::string& fname ) {
	std::string s;
	std::ifstream x(fname);
	for (;;) {
		char b[1024];
		x.getline(b,sizeof(b));
		if (x.eof()) break;
		if (!x) {
			std::string m = (boost::format("error load_file_all %s\n") % fname).str();
			log(m);
			throw std::runtime_error(m);
		}
		s+=b;
		s+="\n";
	}
	return s;
}

#ifdef USE_SSL
static boost::asio::ssl::context ctx{boost::asio::ssl::context::tlsv12};
void load_server_certificate( boost::asio::ssl::context& ctx, const std::string& fname_key, const std::string& fname_cer, const std::string& fname_cer_chain, const std::string& passwd ) {
	// 証明書
	// m_server_name の値は無関係。
	// 証明書記載の common name でアクセスしないと NET::ERR_CERT_COMMON_NAME_INVALID となるので localhost で開いてはダメ。
	// nii-odca4g7rsa.cer を emulsion-labo.physics.aichi-edu.ac.jp.cer の末尾にコピーすれば動作する。

	ctx.set_password_callback([passwd](std::size_t,boost::asio::ssl::context_base::password_purpose) { return passwd.c_str(); });
	ctx.use_private_key_file(fname_key.c_str(),boost::asio::ssl::context::file_format::pem);
	std::string s = load_file_all(fname_cer);
	if ( fname_cer_chain != "-" ) {
		s += "\n";
		s += load_file_all(fname_cer_chain);
	}
	ctx.use_certificate_chain(boost::asio::const_buffer(boost::asio::buffer(s)));
	ctx.set_options(boost::asio::ssl::context::default_workarounds|boost::asio::ssl::context::no_sslv2);
}
#endif

int main( int argc, char** argv ) {
	try {
		{
			std::ifstream f("config.json");
			if ( f.is_open() ) m_cfg = nlohmann::json::parse(f);
		}
		argc--; argv++;
		while ( argc != 0 ) {
			if ( strcmp(*argv,"--ssl") == 0 ) {
				std::string ssl_key,ssl_cer,ssl_cer_chain,ssl_pwd;
				argc--; argv++; m_cfg["ssl-key"] = *argv;
				argc--; argv++; m_cfg["ssl-cer"] = *argv;
				argc--; argv++; m_cfg["ssl-chain"] = *argv;
				argc--; argv++; m_cfg["ssl-pwd"] = *argv;
			}
			else if ( strcmp(*argv,"--port") == 0 ) { argc--; argv++; m_cfg["port"] = atoi(*argv); }
			else if ( strcmp(*argv,"--magic") == 0 ) { argc--; argv++; m_cfg["magic"] = *argv; }
			else throw std::runtime_error((boost::format("unknown option %s\n") % argv).str());
			argc--; argv++;
		}

		if ( m_cfg.contains("port") ) {
			m_port = m_cfg["port"].get<int>();
			m_logfile = ( boost::format("tekuteku-server-%04d.log") % m_port ).str();
		}
		if ( m_cfg.contains("magic") ) m_magic = m_cfg["magic"].get<std::string>();

		truncate_log();
		log(( boost::format("option port=%d\n") % m_port ).str());
		#ifdef USE_SSL
		std::string key = m_cfg["ssl-key"].get<std::string>();
		std::string cer = m_cfg["ssl-cer"].get<std::string>();
		std::string chain = m_cfg["ssl-chain"].get<std::string>();
		std::string pwd = m_cfg["ssl-pwd"].get<std::string>();
		load_server_certificate(ctx,key,cer,chain,pwd);
		#endif

		#ifdef _WINDOWS
		// 同一ポートでの多重起動禁止はトレーの存在確認で行う。
		std::string tray_name = (boost::format("tekuteku-%04d") % m_port).str().c_str();
		if ( tray_exist(tray_name.c_str()) == 1 ) {
			log("stop due to multiple servers\n");
			MessageBoxW(NULL,L"同じポートでは、複数のサーバを動かせません。",L"てくてくノートサーバ",MB_OK);
			return 0;
		}
		if ( m_cfg.contains("exec") ) { for (auto& a : m_cfg["exec"]) boost::process::spawn(a.get<std::string>()); }
		#endif

		m_whiteboard.reserve(4096);
		log((boost::format("started %s\n") % m_version).str());
		if ( enum_network(m_servers) == false ) throw std::runtime_error("enum_network");
		if ( m_servers.empty() ) log("no network\n");
		std::for_each(m_servers.begin(),m_servers.end(),[](const network_t& net){ log((boost::format("server : %s/%s\n") % net.address.to_string() % net.mask.to_string()).str()); });

		thread_r = std::move(std::thread([]{
			auto const ep = boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("0.0.0.0"),m_port};
			#ifndef USE_SSL
			boost::asio::spawn(ioc_r,std::bind(&exec_listen,std::ref(ioc_r),ep,std::placeholders::_1));
			#else
			boost::asio::spawn(ioc_r,std::bind(&exec_listen,std::ref(ioc_r),std::ref(ctx),ep,std::placeholders::_1));
			#endif
			boost::asio::spawn(ioc_r,std::bind(&broadcast_status,std::placeholders::_1));
			ioc_r.run();
			m_takers.clear();
		}));

//		thread_w = std::move(std::thread([]{
//			boost::asio::spawn(ioc_w,std::bind(&broadcast_status,std::placeholders::_1));
//			ioc_w.run();
//		}));

		#ifdef _WINDOWS
		#ifndef USE_SSL
		std::string m_host_url = (boost::format("http://localhost:%d") % m_port).str();	// Chrome でマイク使用ブロックを解除できないので localhost を使用する。
		if (!( reinterpret_cast<uint64_t>(ShellExecute(NULL,"open",m_host_url.c_str(),NULL,NULL,SW_SHOWNORMAL)) > 32 )) throw std::runtime_error("spawn_client");
		#endif
		thread_wd = std::move(std::thread([]{
			while ( wd.wait() ) {
				std::vector<network_t> l;
				if ( enum_network(l) == false ) throw std::runtime_error("enum_network");
				if ( std::equal(l.begin(),l.end(),m_servers.begin(),m_servers.end(),[]( const network_t& a, const network_t& b ){ return a.address == b.address; }) == false ) {
					m_servers.swap(l);
					log("network changed\n");
					if ( m_servers.empty() ) log("no network\n");
					std::for_each(m_servers.begin(),m_servers.end(),[](const network_t& net){ log((boost::format("server : %s/%s\n") % net.address.to_string() % net.mask.to_string()).str()); });
					network_changed = true;
					request_broadcast.set();
				}
				wd.start();
			}
		}));
		#endif
		if ( tray_init((boost::format("tekuteku-%04d") % m_port).str().c_str(),"tekuteku.ico") == 0 ) { while ( tray_loop(1) == 0 ) {} }

		#ifdef _WINDOWS
		wd.stop(); thread_wd.join();
		if ( m_cfg.contains("exec") ) { for (auto& a : m_cfg["exec"]) SendMessage(FindWindow("TRAY",a.get<std::string>().c_str()),WM_CLOSE,0,0); }
		#endif
		terminate_server();
		return 0;
	}
	catch ( boost::system::system_error& e ) { log((boost::format("boost exception : %s\n") % e.what()).str()); }
	catch ( std::exception& e ) { log((boost::format("exception : %s\n") % e.what()).str()); }
	catch ( ... ) { log("unknown exception\n"); }
	#ifdef _WINDOWS
	MessageBoxW(NULL,L"エラーのため終了します。もう一度動かして下さい。",L"てくてくノートサーバ",MB_OK);
	#endif
	return 1;
}
