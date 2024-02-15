#pragma once
#include <time.h>
#include <string>
#include <boost/format.hpp>

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
