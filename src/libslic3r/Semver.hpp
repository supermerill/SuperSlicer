#ifndef slic3r_Semver_hpp_
#define slic3r_Semver_hpp_

#include <string>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <boost/optional.hpp>
#include <boost/format.hpp>

#include "semver/semver.h"

#include "Exception.hpp"

namespace Slic3r {


class Semver
{
public:
	struct Major { const int i;  Major(int i) : i(i) {} };
	struct Minor { const int i;  Minor(int i) : i(i) {} };
	struct Counter { const int i;  Counter(int i) : i(i) {} }; //for SuSi
	struct Patch { const int i;  Patch(int i) : i(i) {} };

	Semver() : ver(semver_zero()) {}

	Semver(int major, int minor, int counter, int patch,
		boost::optional<const std::string&> metadata = boost::none,
		boost::optional<const std::string&> prerelease = boost::none)
		: ver(semver_zero())
	{
		semver_free(&ver);
		ver.counter_size = 4;
		ver.counters = new int[4];
		ver.counters[0] = major;
		ver.counters[1] = minor;
		ver.counters[2] = counter;
		ver.counters[3] = patch;
		set_metadata(metadata);
		set_prerelease(prerelease);
	}

	Semver(const std::string &str) : ver(semver_zero())
	{

		auto parsed = parse(str);
		if (! parsed) {
			throw Slic3r::RuntimeError(std::string("Could not parse version string: ") + str);
		}
		ver = parsed->ver;
		parsed->ver = semver_zero();
	}

	static boost::optional<Semver> parse(const std::string &str)
	{
		semver_t ver = semver_zero();
		if (::semver_parse(str.c_str(), &ver) == 0) {
			return Semver(ver);
		} else {
			return boost::none;
		}
	}

	static const Semver zero() { return Semver(semver_zero()); }

	static const Semver inf()
	{
		semver_t ver = { new int[4], 4, nullptr, nullptr };
		for (int i = 0; i < ver.counter_size; i++)
			ver.counters[i] = std::numeric_limits<int>::max();
		return Semver(ver);
	}

	static const Semver invalid()
	{
		semver_t ver = { new int[1], 1, nullptr, nullptr };
		ver.counters[0] = -1;
		return Semver(ver);
	}

	Semver(Semver &&other) : ver(other.ver) { other.ver = semver_zero(); }
	Semver(const Semver &other) : ver(::semver_copy(&other.ver)) {}

	Semver &operator=(Semver &&other)
	{
		::semver_free(&ver);
		ver = other.ver;
		other.ver = semver_zero();
		return *this;
	}

	Semver &operator=(const Semver &other)
	{
		::semver_free(&ver);
		ver = ::semver_copy(&other.ver);
		return *this;
	}

	~Semver() { ::semver_free(&ver); }

	// const accessors
	//int 		maj()        const { return ver.counter_size > 0 ? ver.counters[0] : 0; }
	//int 		min()        const { return ver.counter_size > 1 ? ver.counters[1] : 0; }
	//int 		counter()    const { return ver.counter_size > 2 ? ver.counters[2] : 0; }
	//int 		patch() 	 const { return ver.counter_size > 3 ? ver.counters[3] : 0; }
	const char*	prerelease() const { return ver.prerelease; }
	const char*	metadata() 	 const { return ver.metadata; }
	
	// Setters
	//void set_maj(int maj) { if(ver.counter_size > 0) ver.counters[0] = maj; }
	//void set_min(int min) { if (ver.counter_size > 1) ver.counters[1] = min; }
	//void set_counter(int count) { if (ver.counter_size > 2) ver.counters[2] = count; }
	//void set_patch(int patch) { if (ver.counter_size > 3) ver.counters[3] = patch; }
	void set_metadata(boost::optional<const std::string&> meta) { ver.metadata = meta ? strdup(*meta) : nullptr; }
	void set_prerelease(boost::optional<const std::string&> pre) { ver.prerelease = pre ? strdup(*pre) : nullptr; }

	// Comparison
	bool operator<(const Semver &b)  const { return ::semver_compare(ver, b.ver) == -1; }
	bool operator<=(const Semver &b) const { return ::semver_compare(ver, b.ver) <= 0; }
	bool operator==(const Semver &b) const { return ::semver_compare(ver, b.ver) == 0; }
	bool operator!=(const Semver &b) const { return ::semver_compare(ver, b.ver) != 0; }
	bool operator>=(const Semver &b) const { return ::semver_compare(ver, b.ver) >= 0; }
	bool operator>(const Semver &b)  const { return ::semver_compare(ver, b.ver) == 1; }
	// We're using '&' instead of the '~' operator here as '~' is unary-only:
	// Satisfies patch if Major and minor are equal.
	bool operator&(const Semver &b) const { return ::semver_satisfies_patch(ver, b.ver) != 0; }
	bool operator^(const Semver &b) const { return ::semver_satisfies_caret(ver, b.ver) != 0; }
	bool in_range(const Semver &low, const Semver &high) const { return low <= *this && *this <= high; }
	bool valid()                    const { return *this != zero() && *this != inf() && *this != invalid(); }

	// Conversion
	std::string to_string() const {
		std::string res;
		for (int i = 0; i < ver.counter_size; i++) {
			res += ( (i==0 ? boost::format("%1%") : boost::format(".%1%")) % ver.counters[i]).str();
		}
		if (ver.prerelease != nullptr) { res += '-'; res += ver.prerelease; }
		if (ver.metadata != nullptr)   { res += '+'; res += ver.metadata; }
		return res;
	}

	// Arithmetics
	//Semver& operator+=(const Major &b) { set_maj(maj()+b.i); return *this; }
	//Semver& operator+=(const Minor &b) { set_min(min() + b.i); return *this; }
	//Semver& operator+=(const Counter& b) { set_counter(counter() + b.i); return *this; }
	//Semver& operator+=(const Patch &b) { set_patch(patch() + b.i); return *this; }
	//Semver& operator-=(const Major& b) { set_maj(maj() - b.i); return *this; }
	//Semver& operator-=(const Minor& b) { set_min(min() - b.i); return *this; }
	//Semver& operator-=(const Counter& b) { set_counter(counter() - b.i); return *this; }
	//Semver& operator-=(const Patch& b) { set_patch(patch() - b.i); return *this; }
	//Semver operator+(const Major &b) const { Semver res(*this); return res += b; }
	//Semver operator+(const Minor &b) const { Semver res(*this); return res += b; }
	//Semver operator+(const Counter& b) const { Semver res(*this); return res += b; }
	//Semver operator+(const Patch& b) const { Semver res(*this); return res += b; }
	//Semver operator-(const Major &b) const { Semver res(*this); return res -= b; }
	//Semver operator-(const Minor &b) const { Semver res(*this); return res -= b; }
	//Semver operator-(const Counter& b) const { Semver res(*this); return res -= b; }
	//Semver operator-(const Patch& b) const { Semver res(*this); return res -= b; }

	// Stream output
	friend std::ostream& operator<<(std::ostream& os, const Semver &self) {
		os << self.to_string();
		return os;
	}
private:
	semver_t ver;


	Semver(semver_t ver) : ver(ver) {}

	static semver_t semver_zero() { return { nullptr, 0, nullptr, nullptr }; }
	static char * strdup(const std::string &str) { return ::semver_strdup(str.data()); }
};


}
#endif
