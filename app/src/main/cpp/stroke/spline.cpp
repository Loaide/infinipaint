#include "stroke/spline.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
namespace engine::stroke {
namespace {
using core::Vec2;
using core::Error;
using core::ErrorCode;
template<class T> void reserve_one(std::vector<T>& v, std::size_t limit) {
    if (v.size() != v.capacity()) return;
    limit = std::min(limit, v.max_size());
    if (v.size() >= limit) throw std::length_error("live stroke limit");
    v.reserve(std::max(v.size()+1, v.size()+std::min(v.size(),limit-v.size())));
}
bool finite(Vec2 p) { return std::isfinite(p.x)&&std::isfinite(p.y); }
bool finite_point(const SplinePoint& p) {
    return finite(p.position)&&std::isfinite(p.timestamp)&&p.timestamp>=0&&
        std::isfinite(p.pressure)&&p.pressure>=0&&p.pressure<=1&&
        std::isfinite(p.tilt_x)&&std::isfinite(p.tilt_y)&&
        std::isfinite(p.speed)&&p.speed>=0;
}
Vec2 add(Vec2 a,Vec2 b){return {a.x+b.x,a.y+b.y};}
Vec2 sub(Vec2 a,Vec2 b){return {a.x-b.x,a.y-b.y};}
Vec2 mul(Vec2 a,double k){return {a.x*k,a.y*k};}
Vec2 cubic(Vec2 a,Vec2 b,Vec2 c,Vec2 d,double u) {return add(a,mul(add(b,mul(add(c,mul(d,u)),u)),u));}
Vec2 deriv(Vec2 b,Vec2 c,Vec2 d,double u) {return add(b,mul(add(mul(c,2),mul(d,3*u)),u));}
double distance(Vec2 a,Vec2 b){return std::hypot(a.x-b.x,a.y-b.y);}
double length_adaptive(Vec2 a,Vec2 b,Vec2 c,Vec2 d,double u0,double u1,Vec2 p0,Vec2 p1,double tol,unsigned depth){
    const double mid=(u0+u1)*0.5;
    const Vec2 pm=cubic(a,b,c,d,mid);
    const double chord=distance(p0,p1), split=distance(p0,pm)+distance(pm,p1);
    if(!std::isfinite(split)||!std::isfinite(chord))return std::numeric_limits<double>::infinity();
    if(depth==0 || split-chord<=tol) return split;
    return length_adaptive(a,b,c,d,u0,mid,p0,pm,tol*0.5,depth-1)+
           length_adaptive(a,b,c,d,mid,u1,pm,p1,tol*0.5,depth-1);
}
double knot_step(Vec2 a,Vec2 b,Parameterization mode){
    const double dist=distance(a,b);
    if(mode==Parameterization::Uniform)return 1.0;
    if(mode==Parameterization::Centripetal)return std::sqrt(dist);
    return dist;
}
// Catmull-Rom derivative with respect to knot parameter, for distinct adjacent points.
// Formula derived from derivatives of nonuniform quadratic interpolation.
Vec2 interior(Vec2 prev,Vec2 cur,Vec2 next,double h0,double h1){
    Vec2 d0=mul(sub(cur,prev),1.0/h0), d1=mul(sub(next,cur),1.0/h1);
    return add(mul(d0,h1/(h0+h1)),mul(d1,h0/(h0+h1)));
}
double interp(double a,double b,double u){return std::lerp(a,b,u);}
}
core::Result<SplineFitter> SplineFitter::create(Params p){
    if(p.max_samples==0||p.max_arc_depth==0||p.max_arc_depth>20||
       !std::isfinite(p.arc_tolerance)||p.arc_tolerance<=0||
       (p.parameterization!=Parameterization::Uniform&&p.parameterization!=Parameterization::Centripetal&&p.parameterization!=Parameterization::Chordal))
        return Error{ErrorCode::InvalidArgument,"Invalid spline parameters"};
    return SplineFitter(p);
}
core::Result<FittedSpline> SplineFitter::fit(const input::CleanInputStroke& clean,const Affine2D& x) const{
    if(!std::isfinite(x.m00)||!std::isfinite(x.m01)||!std::isfinite(x.m10)||!std::isfinite(x.m11)||!std::isfinite(x.tx)||!std::isfinite(x.ty))
        return Error{ErrorCode::NonFinite,"Nonfinite coordinate transform"};
    if(clean.empty())return Error{ErrorCode::EmptyInput,"Empty stroke"};
    if(clean.size()>params_.max_samples)return Error{ErrorCode::ResourceLimit,"Stroke sample limit exceeded"};
    try {
        std::vector<SplinePoint> points; points.reserve(clean.size());
        for(const auto& p:clean){
            if(!finite(p.position)||!std::isfinite(p.timestamp)||!std::isfinite(p.pressure)||!std::isfinite(p.tilt_x)||!std::isfinite(p.tilt_y)||!finite(p.velocity)||!std::isfinite(p.speed))
                return Error{ErrorCode::NonFinite,"Nonfinite input sample"};
            // Use fma to minimize rounding in the affine transform.
            Vec2 w{std::fma(x.m01,p.position.y,std::fma(x.m00,p.position.x,x.tx)),
                   std::fma(x.m11,p.position.y,std::fma(x.m10,p.position.x,x.ty))};
            if(!finite(w))return Error{ErrorCode::NumericalError,"Transform overflow"};
            points.push_back({w,p.timestamp,p.pressure,p.tilt_x,p.tilt_y,p.speed}); // speed remains VIEW-LOCAL pixels/second
        }
        return fit_world(points);
    }catch(const std::bad_alloc&){return Error{ErrorCode::AllocationFailure,"Spline allocation failed"};}
     catch(const std::length_error&){return Error{ErrorCode::ResourceLimit,"Spline allocation limit"};}
}
core::Result<FittedSpline> SplineFitter::fit_world(const std::vector<SplinePoint>& points) const{
    if(points.empty())return Error{ErrorCode::EmptyInput,"Empty stroke"};
    if(points.size()>params_.max_samples)return Error{ErrorCode::ResourceLimit,"Stroke sample limit exceeded"};
    try {
        FittedSpline out;
        out.origin_=points.front().position;
        out.source_=points;
        std::vector<Vec2> unique; std::vector<std::size_t> indices;
        unique.reserve(points.size());indices.reserve(points.size());
        for(std::size_t i=0;i<points.size();++i){
            const auto& p=points[i];
            if(!finite_point(p))return Error{ErrorCode::InvalidArgument,"Invalid world sample"};
            if(i && p.timestamp<points[i-1].timestamp)return Error{ErrorCode::NonMonotonicTime,"Regressing timestamp"};
            const Vec2 local=sub(p.position,out.origin_);
            if(!finite(local))return Error{ErrorCode::NumericalError,"World coordinate difference overflow"};
            if(unique.empty() || local.x!=unique.back().x || local.y!=unique.back().y){unique.push_back(local);indices.push_back(i);}
        }
        if(unique.size()<2)return out; // stationary stroke: retain ALL attribute samples, render as dot later
        const std::size_t n=unique.size();
        std::vector<double> h(n-1);
        for(std::size_t i=0;i+1<n;++i){
            h[i]=knot_step(unique[i],unique[i+1],params_.parameterization);
            if(!std::isfinite(h[i])||h[i]<=0)return Error{ErrorCode::NumericalError,"Invalid spline knot interval"};
        }
        out.segments_.reserve(n-1);
        for(std::size_t i=0;i+1<n;++i){
            Vec2 d0,d1;
            if(i==0)d0=mul(sub(unique[1],unique[0]),1.0/h[0]);
            else d0=interior(unique[i-1],unique[i],unique[i+1],h[i-1],h[i]);
            if(i+2==n)d1=mul(sub(unique[n-1],unique[n-2]),1.0/h[n-2]);
            else d1=interior(unique[i],unique[i+1],unique[i+2],h[i],h[i+1]);
            const Vec2 p0=unique[i],p1=unique[i+1],delta=sub(p1,p0);
            if(!finite(d0)||!finite(d1)||!finite(delta))return Error{ErrorCode::NumericalError,"Spline derivative overflow"};
            const Vec2 m0=mul(d0,h[i]),m1=mul(d1,h[i]);
            // Hermite coefficients; exact position and analytic derivative.
            const Vec2 b=m0;
            const Vec2 c=sub(mul(delta,3),add(mul(m0,2),m1));
            const Vec2 d=add(mul(delta,-2),add(m0,m1));
            if(!finite(b)||!finite(c)||!finite(d))return Error{ErrorCode::NumericalError,"Spline coefficient overflow"};
            const double len=length_adaptive(p0,b,c,d,0,1,p0,cubic(p0,b,c,d,1),params_.arc_tolerance,params_.max_arc_depth);
            if(!std::isfinite(len)||!std::isfinite(out.total_length_+len))return Error{ErrorCode::NumericalError,"Spline length overflow"};
            out.segments_.push_back({p0,b,c,d,indices[i],indices[i+1],len});
            out.total_length_+=len;
        }
        return out;
    }catch(const std::bad_alloc&){return Error{ErrorCode::AllocationFailure,"Spline allocation failed"};}
     catch(const std::length_error&){return Error{ErrorCode::ResourceLimit,"Spline allocation limit"};}
}
core::Result<StrokeSample> FittedSpline::evaluate(std::size_t segment,double u) const noexcept{
    if(!std::isfinite(u)||u<0||u>1)return Error{ErrorCode::InvalidArgument,"Evaluation parameter outside [0,1]"};
    if(source_.empty())return Error{ErrorCode::InvalidState,"Empty spline"};
    if(segments_.empty()){
        if(segment!=0)return Error{ErrorCode::InvalidArgument,"Invalid dot segment index"};
        const double time=std::lerp(source_.front().timestamp,source_.back().timestamp,u);
        return evaluate_at_time(time);
    }
    if(segment>=segments_.size())return Error{ErrorCode::InvalidArgument,"Invalid segment index"};
    const auto& s=segments_[segment];
    const Vec2 local=cubic(s.a,s.b,s.c,s.d,u);
    const Vec2 tangent=deriv(s.b,s.c,s.d,u);
    const Vec2 pos=add(origin_,local);
    if(!finite(pos)||!finite(tangent))return Error{ErrorCode::NumericalError,"Evaluation overflow"};
    const auto& a=source_[s.left];const auto& b=source_[s.right];
    if (a.timestamp==b.timestamp) {
        const auto& end=(u==1 && segment+1==segments_.size()) ? source_.back() : b;
        return StrokeSample{pos,tangent,a.timestamp,interp(a.pressure,end.pressure,u),
            interp(a.tilt_x,end.tilt_x,u),interp(a.tilt_y,end.tilt_y,u),interp(a.speed,end.speed,u)};
    }
    // For the terminal endpoint, include pressure/tilt updates received while
    // stationary after the final geometric knot. The earlier timeline remains
    // accessible through evaluate_at_time().
    const double time=(segment+1==segments_.size() && u==1.0)
        ? source_.back().timestamp : interp(a.timestamp,b.timestamp,u);
    auto attrs=evaluate_at_time(time);
    if(!attrs)return attrs.error();
    auto sample=attrs.value();sample.position=pos;sample.tangent=tangent;
    return sample;
}
core::Result<StrokeSample> FittedSpline::evaluate_at_time(double timestamp) const noexcept{
    if(!std::isfinite(timestamp))return Error{ErrorCode::InvalidArgument,"Nonfinite evaluation time"};
    if(source_.empty())return Error{ErrorCode::InvalidState,"Empty spline"};
    if(timestamp<source_.front().timestamp||timestamp>source_.back().timestamp)
        return Error{ErrorCode::InvalidArgument,"Time outside stroke range"};
    auto it=std::upper_bound(source_.begin(),source_.end(),timestamp,
        [](double t,const SplinePoint& p){return t<p.timestamp;});
    if((it-1)->timestamp==timestamp) {
        const auto& p=*(it-1); // A time-only query returns the last sample at that instant.
        return StrokeSample{p.position,{},p.timestamp,p.pressure,p.tilt_x,p.tilt_y,p.speed};
    }
    const auto& a=*(it-1);const auto& b=*it;
    const double u=(timestamp-a.timestamp)/(b.timestamp-a.timestamp);
    const Vec2 pos{std::lerp(a.position.x,b.position.x,u),std::lerp(a.position.y,b.position.y,u)};
    return StrokeSample{pos,{},timestamp,interp(a.pressure,b.pressure,u),
        interp(a.tilt_x,b.tilt_x,u),interp(a.tilt_y,b.tilt_y,u),interp(a.speed,b.speed,u)};
}
core::Result<StrokeSample> FittedSpline::evaluate(double t) const noexcept{
    if(!std::isfinite(t)||t<0||t>1)return Error{ErrorCode::InvalidArgument,"Evaluation parameter outside [0,1]"};
    if(source_.empty())return Error{ErrorCode::InvalidState,"Empty spline"};
    if(segments_.empty())return evaluate(std::size_t{0},t);
    if(t==1)return evaluate(segments_.size()-1,1);
    const double f=t*static_cast<double>(segments_.size());
    const std::size_t i=static_cast<std::size_t>(f);
    return evaluate(i,f-static_cast<double>(i));
}
core::Result<void> IncrementalSplineBuilder::append(const SplinePoint& p){
    if(!finite_point(p))return Error{ErrorCode::InvalidArgument,"Invalid live sample"};
    if(points_.size()>=fitter_.max_samples())return Error{ErrorCode::ResourceLimit,"Live stroke sample limit"};
    if(!points_.empty() && p.timestamp<points_.back().timestamp)
        return Error{ErrorCode::NonMonotonicTime,"Regressing live timestamp"};
    if(!points_.empty() && !finite(sub(p.position,points_.front().position)))
        return Error{ErrorCode::NumericalError,"Live coordinate difference overflow"};
    const bool new_knot=points_.empty() || p.position.x!=points_.back().position.x || p.position.y!=points_.back().position.y;
    try {
        // Prepare all potentially allocating changes BEFORE committing any state.
        reserve_one(points_,fitter_.max_samples());
        if(new_knot)reserve_one(knot_indices_,fitter_.max_samples());
        FittedSpline next;
        if(new_knot && !points_.empty()){
            const std::size_t start=knot_indices_.size()>3 ? knot_indices_.size()-3 : 0;
            std::vector<SplinePoint> window;
            window.reserve(knot_indices_.size()-start+1);
            for(std::size_t i=start;i<knot_indices_.size();++i)window.push_back(points_[knot_indices_[i]]);
            window.push_back(p);
            auto r=fitter_.fit_world(window);
            if(!r)return r.error();
            next=std::move(r.value());
            // The new knot changes the previously terminal segment and adds one.
            // Earlier segments retain their four geometric neighbors unchanged.
            const std::size_t replace=knot_indices_.size()>1 ? knot_indices_.size()-2 : 0;
            const std::size_t old_segments=cached_.segments_.size();
            if(old_segments!=knot_indices_.size()-1)return Error{ErrorCode::InvalidState,"Incremental cache inconsistent"};
            reserve_one(cached_.segments_,fitter_.max_samples());
            double replaced_length=0;
            for(std::size_t i=replace;i<old_segments;++i)replaced_length+=cached_.segments_[i].length;
            const std::size_t window_offset=replace-start;
            double added_length=0;
            for(std::size_t i=window_offset;i<next.segments_.size();++i)added_length+=next.segments_[i].length;
            // Incremental length accounting may accumulate floating-point
            // rounding; a rare negative result is recovered from segments.
            double total=cached_.total_length_-replaced_length+added_length;
            // A negative total can arise from cancellation even when every
            // segment has nonnegative length. Recover from the segment data.
            if(total<0 && std::isfinite(total)) {
                total=0;
                for(std::size_t i=0;i<replace;++i) total+=cached_.segments_[i].length;
                total+=added_length;
            }
            if(!std::isfinite(total)||total<0)return Error{ErrorCode::NumericalError,"Incremental length overflow"};
            // Translate coefficients to the stable document-local origin.
            const Vec2 shift=sub(next.origin_,cached_.origin_);
            if(!finite(shift))return Error{ErrorCode::NumericalError,"Incremental origin overflow"};
            for(std::size_t i=window_offset;i<next.segments_.size();++i){
                next.segments_[i].a=add(next.segments_[i].a,shift);
                if(!finite(next.segments_[i].a))return Error{ErrorCode::NumericalError,"Incremental coefficient overflow"};
                next.segments_[i].left=knot_indices_[start+i];
                next.segments_[i].right=(start+i+1==knot_indices_.size())?points_.size():knot_indices_[start+i+1];
            }
            // Commit is non-throwing after reserve (POD segments, bounded capacity).
            points_.push_back(p);
            knot_indices_.push_back(points_.size()-1);
            cached_.segments_.resize(replace);
            for(std::size_t i=window_offset;i<next.segments_.size();++i)cached_.segments_.push_back(next.segments_[i]);
            cached_.total_length_=total;
            ++distinct_count_;
            return {};
        }
        // First knot or stationary attribute-only sample: O(1) amortized.
        points_.push_back(p);
        if(new_knot){
            knot_indices_.push_back(points_.size()-1);
            cached_.origin_=p.position;
            ++distinct_count_;
        }
        return {};
    }catch(const std::bad_alloc&){return Error{ErrorCode::AllocationFailure,"Live stroke allocation failed"};}
     catch(const std::length_error&){return Error{ErrorCode::ResourceLimit,"Live stroke size limit"};}
}
core::Result<core::Vec2> IncrementalSplineBuilder::geometry_at(std::size_t segment, double u) const noexcept {
    if (segment >= cached_.segments_.size() || !std::isfinite(u) || u < 0.0 || u > 1.0)
        return Error{ErrorCode::InvalidArgument, "Invalid cached spline segment"};
    const auto& s = cached_.segments_[segment];
    const Vec2 result = add(cached_.origin_, cubic(s.a, s.b, s.c, s.d, u));
    if (!finite(result)) return Error{ErrorCode::NumericalError, "Invalid cached spline position"};
    return result;
}
core::Result<FittedSpline> IncrementalSplineBuilder::snapshot() const{
    if(points_.empty())return Error{ErrorCode::EmptyInput,"Empty live stroke"};
    try {
        FittedSpline result=cached_; // copies cached coefficients, no curve fitting
        result.source_=points_;
        return result;
    }catch(const std::bad_alloc&){return Error{ErrorCode::AllocationFailure,"Snapshot allocation failed"};}
     catch(const std::length_error&){return Error{ErrorCode::ResourceLimit,"Snapshot size limit"};}
}
core::Result<FittedSpline> IncrementalSplineBuilder::tail_preview() const{
    if(points_.empty())return Error{ErrorCode::EmptyInput,"Empty live stroke"};
    // Bounded preview: at most four geometric knots plus the latest attribute sample.
    // Full stationary history remains available through snapshot() and is never refitted.
    try {
        const std::size_t start=knot_indices_.size()>4 ? knot_indices_.size()-4 : 0;
        std::vector<SplinePoint> tail;
        tail.reserve(knot_indices_.size()-start+1);
        for(std::size_t i=start;i<knot_indices_.size();++i)tail.push_back(points_[knot_indices_[i]]);
        if(knot_indices_.back()!=points_.size()-1)tail.push_back(points_.back());
        return fitter_.fit_world(tail);
    }catch(const std::bad_alloc&){return Error{ErrorCode::AllocationFailure,"Preview allocation failed"};}
     catch(const std::length_error&){return Error{ErrorCode::ResourceLimit,"Preview allocation limit"};}
}
}
