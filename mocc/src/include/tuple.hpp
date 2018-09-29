#pragma once

#include <atomic>
#include <cstdint>

#include "lock.hpp"

#define TEMP_THRESHOLD 5
#define TEMP_MAX 20
#define TEMP_RESET_US 100

using namespace std;

struct Tidword {
	union {
		uint64_t obj;
		struct {
			bool lock:1;
			uint64_t tid:31;
			uint64_t epoch:32;
		};
	};

	Tidword() {
		obj = 0;
	}

	bool operator==(const Tidword& right) const {
		return obj == right.obj;
	}

	bool operator!=(const Tidword& right) const {
		return !operator==(right);
	}

	bool operator<(const Tidword& right) const {
		return this->obj < right.obj;
	}
};

class Tuple	{
public:
	unsigned int key = 0;
	unsigned int val;
	Tidword tidword;
	atomic<uint64_t> temp;	//	temprature, min 0, max 20
	uint8_t padding[4];
	RWLock lock;	// 4byte
};

// use for read-write set
class ReadElement {
public:
	Tidword tidword;
	unsigned int key, val;
	bool failed_verification;

	ReadElement (Tidword tidword, unsigned int key, unsigned int val) {
		this->tidword = tidword;
		this->key = key;
		this->val = val;
		this->failed_verification = false;
	}

	bool operator<(const ReadElement& right) const {
		return this->key < right.key;
	}
};
	
class WriteElement {
public:
	unsigned int key, val;

	WriteElement(unsigned int key, unsigned int val) {
		this->key = key;
		this->val = val;
	}

	bool operator<(const WriteElement& right) const {
		return this->key < right.key;
	}
};
