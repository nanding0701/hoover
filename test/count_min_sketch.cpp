# include <iostream>
# include <cmath>
# include <cstdlib>
# include <ctime>
# include <limits>
#include <stdlib.h>
#include <string.h>
# include "count_min_sketch.hpp"
using namespace std;

/**
   Class definition for CountMinSketch.
   public operations:
   // overloaded updates
   void update(int item, int c);
   void update(char *item, int c);
   // overloaded estimates
   unsigned int estimate(int item);
   unsigned int estimate(char *item);
**/


// CountMinSketch constructor
// ep -> error 0.01 < ep < 1 (the smaller the better)
// gamma -> probability for error (the smaller the better) 0 < gamm < 1
CountMinSketch::CountMinSketch(float ep, float gamm) {
  if (!(0.009 <= ep && ep < 1)) {
    cout << "eps must be in this range: [0.01, 1)" << endl;
    exit(EXIT_FAILURE);
  } else if (!(0 < gamm && gamm < 1)) {
    cout << "gamma must be in this range: (0,1)" << endl;
    exit(EXIT_FAILURE);
  }
  eps = ep;
  gamma = gamm;
  w = ceil(exp(1)/eps);
  d = ceil(log(1/gamma));
  total = 0;
  // initialize counter array of arrays, C
  C = new int *[d];
  unsigned int i, j;
  for (i = 0; i < d; i++) {
    C[i] = new int[w];
    for (j = 0; j < w; j++) {
      C[i][j] = 0;
    }
  }
  // initialize d pairwise independent hashes
  srand(time(NULL));
  hashes = new unsigned* [d];
  for (i = 0; i < d; i++) {
    hashes[i] = new unsigned[2];
    genajbj(hashes, i);
  }
}

CountMinSketch::CountMinSketch(const CountMinSketch &other) {
    w = other.w;
    d = other.d;
    eps = other.eps;
    gamma = other.gamma;
    aj = other.aj;
    bj = other.bj;
    total = other.total;

    C = new int *[d];
    unsigned int i, j;
    for (i = 0; i < d; i++) {
        C[i] = new int[w];
        memcpy(C[i], other.C[i], w * sizeof(C[0][0]));
    }

    hashes = new unsigned* [d];
    for (i = 0; i < d; i++) {
        hashes[i] = new unsigned[2];
        memcpy(hashes[i], other.hashes[i], 2 * sizeof(hashes[0][0]));
    }
}

// CountMinSkectch destructor
CountMinSketch::~CountMinSketch() {
  // free array of counters, C
  unsigned int i;
  for (i = 0; i < d; i++) {
    delete[] C[i];
  }
  delete[] C;
  
  // free array of hash values
  for (i = 0; i < d; i++) {
    delete[] hashes[i];
  }
  delete[] hashes;
}

// CountMinSketch totalcount returns the
// total count of all items in the sketch
unsigned int CountMinSketch::totalcount() {
  return total;
}

// countMinSketch update item count (int)
void CountMinSketch::update(unsigned int item, int c) {
  total = total + c;
  unsigned int hashval = 0;
  for (unsigned int j = 0; j < d; j++) {
    hashval = (((long)hashes[j][0]*item+hashes[j][1])%LONG_PRIME)%w;
    C[j][hashval] = C[j][hashval] + c;
  }
}

// countMinSketch update item count (string)
void CountMinSketch::update(const char *str, int c) {
  int hashval = hashstr(str);
  update(hashval, c);
}

// CountMinSketch estimate item count (int)
unsigned int CountMinSketch::estimate(unsigned int item) {
  int minval = numeric_limits<int>::max();
  unsigned int hashval = 0;
  for (unsigned int j = 0; j < d; j++) {
    hashval = (((long)hashes[j][0]*item+hashes[j][1])%LONG_PRIME)%w;
    minval = MIN(minval, C[j][hashval]);
  }
  return minval;
}

// CountMinSketch estimate item count (string)
unsigned int CountMinSketch::estimate(const char *str) {
  int hashval = hashstr(str);
  return estimate(hashval);
}

// generates aj,bj from field Z_p for use in hashing
void CountMinSketch::genajbj(unsigned** hashes, int i) {
  hashes[i][0] = unsigned(float(rand())*float(LONG_PRIME)/float(RAND_MAX) + 1);
  hashes[i][1] = unsigned(float(rand())*float(LONG_PRIME)/float(RAND_MAX) + 1);
}

// generates a hash value for a sting
// same as djb2 hash function
unsigned int CountMinSketch::hashstr(const char *str) {
  unsigned long hash = 5381;
  int c;
  while (c = *str++) {
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  }
  return hash;
}


