// Sustituto mínimo de Arduino.h para compilar fusion.cpp en el computador.
#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
using std::isnan;
template<class T,class L,class H> T constrain(T x,L lo,H hi){return x<lo?lo:(x>hi?hi:x);}
struct IPAddress{IPAddress(int,int,int,int){}};
