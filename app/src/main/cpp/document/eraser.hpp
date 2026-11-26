#pragma once
#include "core/math/vec2.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace engine::document {
using core::Vec2;
struct Interval { double lo=1,hi=0; };
inline double dot(Vec2 a,Vec2 b) { return a.x*b.x+a.y*b.y; }
inline double cross(Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; }
// Intersect an eraser centerline with the convex hull of two pressure discs.
// Expanding both radii by the nib tolerance is the exact swept-disc hit shape.
inline Interval clipInk(Vec2 a,Vec2 b,Vec2 p,Vec2 q,double r,double s) {
    Vec2 path=b-a,axis=q-p;
    double length=axis.length(),pathLength=path.length();
    Interval result;
    auto add=[&](double lo,double hi) {
        lo=std::max(0.,lo);hi=std::min(1.,hi);
        if(lo<=hi) {result.lo=std::min(lo,result.lo);result.hi=std::max(hi,result.hi);}
    };
    auto disc=[&](Vec2 center,double radius) {
        auto w=center-a;
        if(pathLength==0) {if(w.length()<=radius)add(0,1);return;}
        auto unit=path/pathLength;
        double perp=std::abs(cross(w,unit));
        if(perp>radius) return;
        double along=dot(w,unit),extent=std::sqrt((radius-perp)*(radius+perp));
        add((along-extent)/pathLength,(along+extent)/pathLength);
    };
    disc(p,r);disc(q,s);
    if(length<=std::abs(s-r)) return result;
    Vec2 u=axis/length,normal{-u.y,u.x};
    double cos=(r-s)/length,sin=std::sqrt(std::max(0.,1-cos*cos));
    Vec2 n1=u*cos-normal*sin,n2=u*cos+normal*sin;
    std::array<Vec2,4> hull{p+n1*r,q+n1*s,q+n2*s,p+n2*r};
    double lo=0,hi=1;
    for(size_t i=0;i<4;i++) {
        Vec2 edge=hull[(i+1)%4]-hull[i];
        double offset=cross(edge,a-hull[i]),slope=cross(edge,path);
        if(slope>0) lo=std::max(lo,-offset/slope);
        else if(slope<0) hi=std::min(hi,-offset/slope);
        else if(offset<0) return result;
    }
    add(lo,hi);return result;
}
// Union of centerline intervals already covered by later (higher) strokes.
inline bool expose(std::map<double,double>& cover,Interval i) {
    if(i.hi<i.lo) return false;
    auto it=cover.upper_bound(i.lo);
    if(it!=cover.begin()) {
        auto left=std::prev(it);
        if(left->second>=i.hi) return false;
        if(left->second>=i.lo) {i.lo=left->first;cover.erase(left);}
    }
    while(it!=cover.end() && it->first<=i.hi) {i.hi=std::max(i.hi,it->second);it=cover.erase(it);}
    cover.emplace(i.lo,i.hi);
    return true;
}
}
