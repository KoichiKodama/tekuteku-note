#ifdef _WINDOWS
	#include <SDKDDKVer.h>
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
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
#include <ctime>
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
#include <regex>

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
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/start_dir.hpp>

#include <json.hpp>
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
static std::string m_version = "build 2025-07-16";
static std::string m_server_name = "tekuteku-server";
static std::string m_magic;
static std::string m_logfile = "tekuteku-server.log";
static std::mutex m_mutex_log;
static int debug_async_accept = 0;
static int debug_async_read = 0;
static int debug_write = 0;
static int debug_write_full = 0;
static int debug_whiteboard_update = 0;
static int debug_find_count = 0;

std::string k_date_time( int days_off = 0 ) {
	tzset();
	time_t t = time(nullptr);
	tm* l = localtime(&t);
	if ( days_off != 0 ) {
		l->tm_mday -= days_off;
		t = mktime(l);
		l = localtime(&t);
	}
	return ( boost::format("%04d-%02d-%02d %02d:%02d:%02d") % (l->tm_year+1900) % (l->tm_mon+1) % (l->tm_mday) % (l->tm_hour) % (l->tm_min) % (l->tm_sec) ).str();
}

void truncate_log() {
	if ( std::filesystem::exists(m_logfile) == false ) return;
	std::string fname_old = m_logfile+".old";
	if ( std::filesystem::copy_file(m_logfile,fname_old,std::filesystem::copy_options::overwrite_existing) == false ) return;
	std::ifstream ifs(fname_old);
	std::ofstream ofs(m_logfile,std::ios_base::trunc);
	if (!ifs) return;
	if (!ofs) return;
	std::string date_min = k_date_time(7);	// 7日以前のログを削除する
	std::regex r(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})");
	std::string s;
	while ( std::getline(ifs,s) ) {
		if ( std::regex_search(s,r) == false ) continue;
		if ( s.compare(0,date_min.length(),date_min) > 0 ) ofs << s << "\n";
	}
	ifs.close();
	ofs.close();
	std::filesystem::remove(fname_old);
}

bool log( const std::string& message, bool truncate = false ) {
	std::ios_base::openmode mode = ( truncate ? std::ios_base::trunc : std::ios_base::app );
	std::ofstream out(m_logfile,mode);
	if (!out) return false;
	out << k_date_time() << " " << message;
	return true;
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
	int edit;	// 音声認識結果を手編集した場合の対応用 ( 0:編集無, 1:編集中, 2:編集済 )
	bool tobe_sent;
};

std::map<std::shared_ptr<websocket_stream_t>,taker_info_t> m_takers;
std::vector<whiteboard_element_t> m_whiteboard;
boost::asio::ip::port_type m_port = DEFAULT_PORT;
std::vector<network_t> m_servers;
int num_connected = 0;	// 延べ接続テイカー
int whiteboard_updated_index = 0;
bool whiteboard_updated = false;
bool network_changed = false;

std::vector<std::string> split( const std::string& x ) {
	std::vector<std::string> l;
	std::stringstream ss(x);
	std::string e;
	while ( std::getline(ss,e,'\n') ) { if (!e.empty()) l.push_back(e); }
	return l;
}

boost::beast::flat_buffer copy_to_buffer( const std::string& s ) {
	boost::beast::flat_buffer b;
	size_t n = boost::asio::buffer_copy(b.prepare(s.size()),boost::asio::buffer(s));
	b.commit(n);
	return b;
}

bool log_whiteboard() {
	if ( m_whiteboard.empty() ) return true;
	std::ofstream out(m_logfile,std::ios_base::app);
	if (!out) return false;
	const std::string t = k_date_time()+" ";
	out << t << boost::format("==== whiteboard (%d) ====\n") % m_whiteboard.size();
	std::for_each(m_whiteboard.begin(),m_whiteboard.end(),[&out,t]( const auto& c ){
		const std::vector<std::string> l = split(c.text);
		std::for_each(l.begin(),l.end(),[&out,t]( const auto& s ){ out << t << s << "\n"; });
	});
	out << t << "==== whiteboard ====\n";
	return true;
}

class request_broadcast_event_t {
public:
	request_broadcast_event_t() : m_count(0),m_interval(200),m_stop(false) {};
	virtual ~request_broadcast_event_t() {};
	void set_interval( int msec ) { m_interval = msec; };
	void set() { m_count++; };
	void stop() { m_stop = true; set(); };
	bool wait( boost::asio::yield_context yield ) {
		boost::asio::steady_timer m_timer{yield.get_executor()};
		for (;;) {
			if ( m_count != 0 ) { m_count = 0; break; }
			m_timer.expires_after(boost::asio::chrono::milliseconds(m_interval));
			boost::system::error_code ec;
			m_timer.async_wait(yield[ec]);
		}
		return ( m_stop == true ? false : true );
	};
	bool is_stopped() const { return m_stop; };
	int count() const { return m_count; };
private:
	int m_count;
	int m_interval;
	bool m_stop;
};
boost::asio::io_context ioc_x(6);
static std::thread thread_x{};
request_broadcast_event_t request_broadcast;

void broadcast_status( boost::asio::yield_context yield ) {
	nlohmann::json j_whiteboard = nlohmann::json::array();
	nlohmann::json j_whiteboard_full = nlohmann::json::array();

	while (true) {
		if ( request_broadcast.wait(yield) == false ) break;

		if ( whiteboard_updated == true ) {
			j_whiteboard.clear();
			j_whiteboard_full.get_ref<std::vector<nlohmann::json>&>().resize(m_whiteboard.size());
			for (int i=whiteboard_updated_index;i<m_whiteboard.size();i++) {
				auto& c = m_whiteboard[i];
				if ( c.tobe_sent == true ) {
					nlohmann::json x;
					x["text"] = c.text;
					x["i"] = i;
					x["edit"] = c.edit;
					j_whiteboard.emplace_back(x);
					j_whiteboard_full[i] = x;
					c.tobe_sent = false;
				}
			}
			whiteboard_updated_index = m_whiteboard.size();
			whiteboard_updated = false;
			debug_whiteboard_update++;
		}
		else j_whiteboard.clear();

		std::map<std::shared_ptr<websocket_stream_t>,taker_info_t> x_takers;
		nlohmann::json j_takers;
		for (auto& e:m_takers) {
			std::shared_ptr<websocket_stream_t> p_ws = e.first;
			const taker_info_t t = e.second;
			if ((p_ws)&&(p_ws->is_open())) {
				nlohmann::json x;
				if (!t.is_readonly) x["text"] = t.text;
				x["id"] = t.id;
				x["num"] = t.num;
				j_takers.emplace_back(x);
				x_takers.insert(e);
			}
		}

		for (auto& e:x_takers) {
			std::shared_ptr<websocket_stream_t> p_ws = e.first;
			taker_info_t t = e.second;
			if ((p_ws)&&(p_ws->is_open())) {
				nlohmann::json x;
				x["type"] = 1;
				x["takers"] = j_takers;
				x["whiteboard_size"] = j_whiteboard_full.size();
				if ( t.is_init == true ) {
					x["whiteboard"] = j_whiteboard_full;
					t.is_init = false; debug_write_full++;
				}
				else x["whiteboard"] = j_whiteboard;
				boost::beast::flat_buffer b = copy_to_buffer(x.dump());
				boost::system::error_code ec;
				p_ws->async_write(b.data(),yield[ec]); debug_write++;
				if (ec) log((boost::format("write error %s (%s)\n") % t.id % ec.message()).str());
			}
			else log((boost::format("write skipped %s\n") % t.id).str());
		}

		#ifndef USE_SSL
		if ( network_changed == true ) {
			network_changed = false;
			const std::string localhost = "127.0.0.1";
			for (auto& e:m_takers) {
				const taker_info_t& info = e.second;
				if ( info.id.compare(0,localhost.size(),localhost) == 0 ) {
					std::shared_ptr<websocket_stream_t> p_ws = e.first;
					nlohmann::json x;
					x["type"] = 0;
					x["id"] = info.id;
					x["num"] = info.num;
					x["server"] = nlohmann::json::array();
					for (auto ii=m_servers.begin();ii!=m_servers.end();ii++) {
						const std::string a = (*ii).address.to_string();
						x["server"].push_back(( m_port == DEFAULT_PORT ? a : ( boost::format("%s:%d") % a % m_port ).str() ));
					}
					boost::beast::flat_buffer b = copy_to_buffer(x.dump());
					boost::system::error_code ec;
					p_ws->async_write(b.data(),yield[ec]); debug_write++;
					if (ec) log((boost::format("error notify network change to %s (%s)\n") % info.id % ec.message()).str());
				}
			}
		}
		#endif
	}
}

void exec_websocket_session( std::shared_ptr<websocket_stream_t> p_ws, boost::beast::http::request<boost::beast::http::string_body> req, boost::asio::yield_context yield ) {
	taker_info_t info;
	try {
		boost::system::error_code ec;
		p_ws->async_accept(req,yield); debug_async_accept++;
		auto ep = boost::beast::get_lowest_layer(*p_ws).socket().remote_endpoint(ec);
		info.id = ( boost::format("%s:%d") % ep.address().to_string() % ep.port() ).str();
		info.num = num_connected++;
		info.is_init = true;

		nlohmann::json r;
		r["type"] = 0;
		r["id"] = info.id;
		r["num"] = info.num;
		r["server"] = nlohmann::json::array();
		for (auto ii=m_servers.begin();ii!=m_servers.end();ii++) {
			const std::string a = (*ii).address.to_string();
			r["server"].push_back(( m_port == DEFAULT_PORT ? a : ( boost::format("%s:%d") % a % m_port ).str() ));
		}
		if ( m_cfg.contains("yyprobe") ) r["yyprobe"] = m_cfg["yyprobe"];
		if ( m_cfg.contains("vosk") ) r["vosk"] = m_cfg["vosk"];
		if ( m_cfg.contains("amivoice") ) r["amivoice"] = m_cfg["amivoice"];
		if ( m_cfg.contains("google-translate") ) r["google-translate"] = m_cfg["google-translate"];
		boost::beast::flat_buffer b = copy_to_buffer(r.dump());
		p_ws->async_write(b.data(),yield[ec]); debug_write++;	// m_takers 未登録なので broadcast_status とは干渉しない。
		m_takers[p_ws] = info;
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
			// SSL 接続で ec == boost::asio::ssl::error::stream_truncated を除外した為に無限ループになったと推測。
			if (ec) {
				log((boost::format("read error %s (%s)\n") % info.id % ec.message()).str());
				break;
			}
			std::string s = boost::beast::buffers_to_string(buffer.data());
			if ( s.empty() == false ) {
				nlohmann::json json_i = nlohmann::json::parse(s);
				int status = json_i["status"];
				if ( status == 8 ) {
					log((boost::format("clear whiteboard by %s\n") % info.id).str());
					log_whiteboard();
					m_whiteboard.clear();
					std::for_each(m_takers.begin(),m_takers.end(),[]( auto& e ){ e.second.whiteboard_voice_index = 0; });
					whiteboard_updated_index = 0;
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
						else if ( text_index >= 0 ) {
							if ( text_index < m_whiteboard.size() ) {
								auto& c = m_whiteboard[text_index];
								c.text = text;
								c.edit = 2;
								c.tobe_sent = true;
								whiteboard_updated_index = std::min(whiteboard_updated_index,text_index);
							}
							else m_whiteboard.push_back(whiteboard_element_t(text,-1,info.num)); // 未確定の音声認識結果で削除されたものを編集した場合
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
							whiteboard_updated_index = std::min(whiteboard_updated_index,text_index);
							whiteboard_updated = true;
						}
					}
				}
				else info.is_readonly = true;
				if ( json_i.contains("voice_text") == true && json_i["voice_text"].empty() == false ) {
					auto& l = json_i["voice_text"];
					auto ii = m_whiteboard.begin()+info.whiteboard_voice_index;
					if ( json_i.contains("voice_fixed") ) {
						const int id_fixed = json_i["voice_fixed"];
						auto jj = std::find_if(ii,m_whiteboard.end(),[id_fixed,&info]( const auto& c ){ return ( c.num == info.num && c.id == id_fixed ); });
						if ( jj != m_whiteboard.end() ) info.whiteboard_voice_index = std::distance(m_whiteboard.begin(),++jj);
					}
					for (int j=0;j<l.size();j++) {
						const auto& x = l[j];
						const int id = x["id"];
						const bool is_final = x["final"];
						std::string text = x["text"].get<std::string>() + ( is_final ? "" : "..." );
						ii = std::find_if(ii,m_whiteboard.end(),[id,&info]( const auto& c ){ return ( c.num == info.num && c.id == id ? true : false ); });
						if ( ii == m_whiteboard.end() ) {
							ii = m_whiteboard.insert(ii,whiteboard_element_t(text,id,info.num));
						}
						else {
							auto& c = (*ii);
							if ( c.edit == 0 && c.text != text ) {
								c.text = text;
								c.tobe_sent = true;
								whiteboard_updated_index = std::min(whiteboard_updated_index,static_cast<int>(std::distance(m_whiteboard.begin(),ii)));
							}
						}
						ii++;
					}
					debug_find_count++;
					if ( json_i.contains("voice_last") ) {
						const int id_last = json_i["voice_last"];
						for (;ii!=m_whiteboard.end();) { if ( (*ii).num == info.num && (*ii).id > id_last ) { ii = m_whiteboard.erase(ii); } else { ii++; } }
					}
					whiteboard_updated = true;
				}
				request_broadcast.set();
			}
		}
		log((boost::format("session stop %s\n") % info.id).str());
	}
	catch ( boost::system::system_error& e ) { log((boost::format("boost exception in exec_websocket_session %s : %s\n") % info.id % e.what()).str()); }
	catch ( std::exception& e ) { log((boost::format("exception in exec_websocket_session %s : %s\n") % info.id % e.what()).str()); }
	m_takers.erase(p_ws);
	log((boost::format("session terminated total=%d\n") % m_takers.size()).str());
	request_broadcast.set();
}


boost::beast::string_view mime_type( const std::string& path_ex ) {
	if ( boost::beast::iequals(path_ex,"html")) return "text/html";
	if ( boost::beast::iequals(path_ex,"css" )) return "text/css";
	if ( boost::beast::iequals(path_ex,"txt" )) return "text/plain";
	if ( boost::beast::iequals(path_ex,"js"  )) return "application/javascript";
	if ( boost::beast::iequals(path_ex,"json")) return "application/json";
	if ( boost::beast::iequals(path_ex,"png" )) return "image/png";
	if ( boost::beast::iequals(path_ex,"jpg" )) return "image/jpeg";
	if ( boost::beast::iequals(path_ex,"gif" )) return "image/gif";
	if ( boost::beast::iequals(path_ex,"bmp" )) return "image/bmp";
	if ( boost::beast::iequals(path_ex,"ico" )) return "image/vnd.microsoft.icon";
	if ( boost::beast::iequals(path_ex,"svg" )) return "image/svg+xml";
	return "application/text";
}

struct reply_t {
	tcp_stream_t& m_stream;
	boost::asio::yield_context m_yield;

	reply_t( tcp_stream_t& stream, boost::asio::yield_context yield ) : m_stream(stream),m_yield(yield) {};
	template<class body> void operator()( boost::beast::http::response<body>&& msg ) const {
//		if ( msg.need_eof() ) log("unexpected need_eof() = true in reply_t\n");
		boost::beast::http::response_serializer<body> s{msg};
		boost::beast::error_code ec;
		boost::beast::http::async_write(m_stream,s,m_yield[ec]);
		if (ec) log((boost::format("reply_t write error %s\n") % ec.message()).str());
	};
};

class url_t {
public:
	url_t( const std::string url ) { parse(url); };
	virtual ~url_t() {};
	void parse( const std::string& url ) {
		std::string::size_type i_query = url.find("?");
		std::string::size_type i_anchor = url.find("#");
		m_path = url.substr(0,i_query);
		if ( m_path.back() == '/' ) m_path += "index.html";
		std::string query = ( i_query == std::string::npos ? "" : url.substr(i_query+1,i_anchor) );
		std::string anchor = ( i_anchor == std::string::npos ? "" : url.substr(i_anchor+1) );
		m_path_ex = ( m_path.find_last_of(".") == std::string::npos ? "" : m_path.substr(m_path.find_last_of(".")+1) );

		m_params.clear();
		std::string::size_type ii = 0;
		std::string::size_type jj = query.find_first_of("&"); if ( jj == std::string::npos ) jj = query.size();
		while ( ii < query.size() ) {
			std::string a = query.substr(ii,jj-ii);
			std::string::size_type i = a.find("=");
			m_params[a.substr(0,i)] = ( i == std::string::npos ? "" : a.substr(i+1) );
			ii = jj+1;
			jj = query.find_first_of("&",ii);
			if ( jj == std::string::npos ) jj = query.size();
		}
	};
	std::string path() const { return m_path; };
	std::string path_ex() const { return m_path_ex; };
	std::vector<std::string> params() const {
		std::vector<std::string> r;
		std::for_each(m_params.begin(),m_params.end(),[&r]( const auto& c ){ r.emplace_back(c.first+"="+c.second); });
		return r;
	};
	std::string param( const std::string& key ) const {
		auto ii = m_params.find(key);
		return ( ii == m_params.end() ? "" : (*ii).second );
	};

private:
	std::string m_path;
	std::string m_path_ex;
	std::map<std::string,std::string> m_params;
};

bool is_accessible( const std::string& f ) {
	if ( f == "/index.html" ) return true;
	if ( f == "/tekuteku.ico" ) return true;
	if ( f == "/emoji-smile.svg" ) return true;
	if ( f.substr(0,6) == "/tools" ) return true;
	if ( f == "/pi.html" ) return true;
	if ( f == "/pi-control.sh" ) return true;
	if ( f == "/ssl-keys/tekuteku-pi.crt" ) return true;
	return false;
}

void exec_http_session( tcp_stream_t& stream, boost::asio::yield_context yield ) {
	boost::beast::error_code ec;
	boost::asio::ip::tcp::socket& socket = boost::beast::get_lowest_layer(stream).socket();
	boost::asio::ip::tcp::socket::endpoint_type ep = socket.remote_endpoint(ec);
	#ifdef USE_SSL
    stream.async_handshake(boost::asio::ssl::stream_base::server,yield[ec]);
	#endif
	boost::beast::flat_buffer buffer;
	boost::beast::http::request<boost::beast::http::string_body> req;

	auto response = [&req]( boost::beast::http::status status, const std::string& msg ){
		boost::beast::http::response<boost::beast::http::string_body> res{status,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.set(boost::beast::http::field::content_type,"text/html");
		res.keep_alive(req.keep_alive());
		res.body() = msg;
		res.prepare_payload();
		return res;
	};
	auto bad_request = [&response]( const std::string& msg ){ return response(boost::beast::http::status::bad_request,msg); };
	auto not_found = [&response]( const std::string& msg ){ return response(boost::beast::http::status::not_found,msg); };
	auto internal_error = [&response]( const std::string& msg ){ return response(boost::beast::http::status::internal_server_error,msg); };
	reply_t reply{stream,yield};

	boost::beast::http::async_read(stream,buffer,req,yield[ec]);
	if ( ec == boost::beast::http::error::end_of_stream ) {
		socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send,ec);
		return;
	}
	if (ec) return;

	url_t url(req.target());

	if ( boost::beast::websocket::is_upgrade(req) ) {
		// magic 確認は websocket に限定
		if ( m_magic.empty() == false && url.param("magic") != m_magic ) return reply(bad_request("authentication failure"));
		auto p_ws = std::make_shared<websocket_stream_t>(std::move(stream));
		boost::beast::websocket::stream_base::timeout opt {std::chrono::seconds(5),std::chrono::seconds(30),true};
		p_ws->set_option(opt);
		boost::asio::spawn(boost::beast::get_lowest_layer(*p_ws).socket().get_executor(),std::bind(&exec_websocket_session,p_ws,req,std::placeholders::_1));
		return;
	}

	if ( req.method() != boost::beast::http::verb::get && req.method() != boost::beast::http::verb::head ) return reply(bad_request("unsupported http-method"));
	if ( is_accessible(url.path()) == false ) {
		log((boost::format("exec_http_session rejected '%s'\n") % url.path()).str());
		return reply(not_found(url.path()));
	}

	if ( url.path_ex() == "sh" ) {
		boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::ok,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.keep_alive(req.keep_alive());

		auto cwd = boost::process::v2::process_start_dir(std::filesystem::current_path().string());
		boost::process::v2::popen c(yield.get_executor(),"."+url.path(),url.params(),cwd);
		std::string x;
		for (;;) {
			boost::system::error_code ec;
			std::string t;
			boost::asio::async_read(c,boost::asio::dynamic_buffer(t),yield[ec]);
			x += t;
			if (ec) {
				if ( ec == boost::asio::error::eof ) break;
				log((boost::format("exec_http_session process %s error %s\n") % url.path() % ec.message()).str());
				return reply(internal_error((boost::format("error %s") % ec.message()).str()));
			}
		}
		x = x.substr(x.find("\n\n")+2);
		res.set(boost::beast::http::field::content_type,"text/plain");
		res.content_length(x.size());
		res.body() = std::move(x);
		return reply(std::move(res));
	}
	else {
		boost::beast::http::response<boost::beast::http::file_body> res{boost::beast::http::status::ok,req.version()};
		res.set(boost::beast::http::field::server,m_server_name);
		res.keep_alive(req.keep_alive());
		std::string file_path = "." + url.path();
		res.body().open(file_path.c_str(),boost::beast::file_mode::scan,ec);
		if ( ec == boost::system::errc::no_such_file_or_directory ) return reply(not_found(url.path()));
		if (ec) return reply(internal_error(url.path()+'/'+ec.message()));
		res.set(boost::beast::http::field::content_type,mime_type(url.path_ex()));
		res.content_length(res.body().size());
		if ( req.method() == boost::beast::http::verb::get ) return reply(std::move(res));
		boost::beast::http::response<boost::beast::http::empty_body> res_x{res};
		return reply(std::move(res_x));
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

void terminate_server() {
	request_broadcast.stop();
	ioc_x.stop(); thread_x.join();
	if (true) log_whiteboard();
	log((boost::format("debug_async_accept = %d\n") % debug_async_accept).str());
	log((boost::format("debug_async_read = %d\n") % debug_async_read).str());
	log((boost::format("debug_write = %d\n") % debug_write).str());
	log((boost::format("debug_write_full = %d\n") % debug_write_full).str());
	log((boost::format("debug_whiteboard_update = %d\n") % debug_whiteboard_update).str());
	log((boost::format("debug_find_count = %d\n") % debug_find_count).str());
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
void load_server_certificate( boost::asio::ssl::context& ctx, const std::string& fname_key, const std::string& fname_crt, const std::string& fname_chain, const std::string& passwd ) {
	// 証明書
	// m_server_name の値は無関係。
	// 証明書記載の common name でアクセスしないと NET::ERR_CERT_COMMON_NAME_INVALID となるので localhost で開いてはダメ。
	// nii-odca4g7rsa.cer を emulsion-labo.physics.aichi-edu.ac.jp.cer の末尾にコピーすれば動作する。

	if ( passwd != "-" ) ctx.set_password_callback([passwd](std::size_t,boost::asio::ssl::context_base::password_purpose) { return passwd.c_str(); });
	ctx.use_private_key_file(fname_key.c_str(),boost::asio::ssl::context::file_format::pem);
	std::string s = load_file_all(fname_crt);
	if ( fname_chain != "-" ) {
		s += "\n";
		s += load_file_all(fname_chain);
	}
	ctx.use_certificate_chain(boost::asio::const_buffer(boost::asio::buffer(s)));
	ctx.set_options(boost::asio::ssl::context::default_workarounds|boost::asio::ssl::context::no_sslv2);
}
#endif

void check_network( boost::asio::io_context& ioc, boost::asio::yield_context yield ) {
	boost::system::error_code ec;
	boost::asio::steady_timer timer{ioc};
	while (true) {
		timer.expires_after(boost::asio::chrono::seconds(30));
		timer.async_wait(yield[ec]);

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
	}
}

int main( int argc, char** argv ) {
	try {
		std::string fname_config = "config.json";
		argc--; argv++;
		while ( argc != 0 ) {
			if ( strcmp(*argv,"--config") == 0 ) { argc--; argv++; fname_config = *argv; }
			else throw std::runtime_error((boost::format("unknown option %s\n") % argv).str());
			argc--; argv++;
		}
		{
			std::ifstream f(fname_config);
			if ( f.is_open() ) m_cfg = nlohmann::json::parse(f);
		}

		if ( m_cfg.contains("port") ) {
			m_port = m_cfg["port"].get<int>();
			m_logfile = ( boost::format("tekuteku-server-%04d.log") % m_port ).str();
		}
		if ( m_cfg.contains("magic") ) m_magic = m_cfg["magic"].get<std::string>();
		if ( m_cfg.contains("broadcast_interval") ) request_broadcast.set_interval(m_cfg["broadcast_interval"].get<int>());

		truncate_log();
		#ifdef USE_SSL
		std::string key = m_cfg["ssl"][0].get<std::string>();
		std::string crt = m_cfg["ssl"][1].get<std::string>();
		std::string chain = m_cfg["ssl"][2].get<std::string>();
		std::string pwd = m_cfg["ssl"][3].get<std::string>();
		load_server_certificate(ctx,key,crt,chain,pwd);
		#endif

		std::vector<boost::process::v2::process> m_exec;
		#ifdef _WINDOWS
		// 同一ポートでの多重起動禁止はトレーの存在確認で行う。
		std::string tray_name = (boost::format("tekuteku-%04d") % m_port).str().c_str();
		if ( tray_exist(tray_name.c_str()) == 1 ) {
			log("stop due to multiple servers\n");
			MessageBoxW(NULL,L"同じポートでは、複数のサーバを動かせません。",L"てくてくノートサーバ",MB_OK);
			return 0;
		}
//		if ( m_cfg.contains("exec") ) { for (auto& a : m_cfg["exec"]) boost::process::spawn(a.get<std::string>()); }
		if ( m_cfg.contains("exec") ) { for (auto& a:m_cfg["exec"]) {
			std::string exe = "./"+a.get<std::string>()+".exe";
			m_exec.emplace_back(boost::process::v2::process(ioc_x,exe,{}));
		}}
		#endif

		m_whiteboard.reserve(4096);
		log((boost::format("started %s on port=%d\n") % m_version % m_port).str());
		if ( enum_network(m_servers) == false ) throw std::runtime_error("enum_network");
		if ( m_servers.empty() ) log("no network\n");
		std::for_each(m_servers.begin(),m_servers.end(),[](const network_t& net){ log((boost::format("server : %s/%s\n") % net.address.to_string() % net.mask.to_string()).str()); });

		thread_x = std::move(std::thread([]{
			try {
				auto const ep = boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("0.0.0.0"),m_port};
				#ifndef USE_SSL
				boost::asio::spawn(ioc_x,std::bind(&exec_listen,std::ref(ioc_x),ep,std::placeholders::_1));
				#else
				boost::asio::spawn(ioc_x,std::bind(&exec_listen,std::ref(ioc_x),std::ref(ctx),ep,std::placeholders::_1));
				#endif
				boost::asio::spawn(ioc_x,std::bind(&broadcast_status,std::placeholders::_1));
				boost::asio::spawn(ioc_x,std::bind(&check_network,std::ref(ioc_x),std::placeholders::_1));
				ioc_x.run();
				m_takers.clear();
			}
			catch ( boost::system::system_error& e ) { log((boost::format("boost exception in thread_x : %s\n") % e.what()).str()); }
		}));

		#ifdef _WINDOWS
		#ifndef USE_SSL
		std::string m_host_url = (boost::format("http://localhost:%d") % m_port).str();	// Chrome でマイク使用ブロックを解除できないので localhost を使用する。
		if (!( reinterpret_cast<uint64_t>(ShellExecute(NULL,"open",m_host_url.c_str(),NULL,NULL,SW_SHOWNORMAL)) > 32 )) throw std::runtime_error("spawn_client");
		#endif
		#endif

		if ( tray_init((boost::format("tekuteku-%04d") % m_port).str().c_str(),"tekuteku.ico") == 0 ) { while ( tray_loop(1) == 0 ) {} }

		#ifdef _WINDOWS
		if ( m_cfg.contains("exec") ) { for (auto& a : m_cfg["exec"]) SendMessage(FindWindow("TRAY",a.get<std::string>().c_str()),WM_CLOSE,0,0); }
//		for (auto& proc:m_exec) { proc.terminate(); }
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
