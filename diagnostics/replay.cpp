#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include "input/input_processor.hpp"
#include "stroke/spline.hpp"
using Clock=std::chrono::steady_clock;
static double micros(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::micro>(b-a).count();}
struct Sample { int id; long long ns; double x,y; float p,tx,ty; };
int main(int argc,char**argv){
 if(argc<2){std::cerr<<"Usage: replay <ip_raw_*.csv> [output.csv]\n";return 2;}
 std::ifstream in(argv[1]);if(!in){std::cerr<<"Cannot read input\n";return 2;}
 std::ofstream out(argc>2?argv[2]:"replay_points.csv");
 out<<"pointer_id,sample_ns,raw_x,raw_y,filtered_x,filtered_y,filter_error_px,spline_x,spline_y,spline_error_px\n";
 auto processor=engine::input::InputProcessor::create({25.0,0.12,8.0});
 auto fitter=engine::stroke::SplineFitter::create();
 if(!processor||!fitter){std::cerr<<"Cannot create processors\n";return 3;}
 std::vector<Sample> rows;std::string line;std::getline(in,line);
 while(std::getline(in,line)){
  std::stringstream ss(line);std::string fields[11];bool ok=true;
  for(auto &f:fields){if(!std::getline(ss,f,',')){ok=false;break;}}
  if(!fields[10].empty() && fields[10].back()=='\r') fields[10].pop_back();
  if(!ok||(fields[10]!="ACCEPTED" && fields[10]!="ACCEPTED_SAME_TIMESTAMP"))continue;
  try {rows.push_back({std::stoi(fields[0]),std::stoll(fields[2]),std::stod(fields[4]),std::stod(fields[5]),
    std::stof(fields[6]),std::stof(fields[7]),std::stof(fields[8])});}catch(...){}
 }
 if(rows.empty()){std::cerr<<"No accepted samples\n";return 4;}
 std::stable_sort(rows.begin(),rows.end(),[](auto&a,auto&b){return a.ns<b.ns;});
 // This replay processes one pointer at a time; multiple pointers are grouped by id.
 std::vector<int> ids;for(auto&r:rows)if(std::find(ids.begin(),ids.end(),r.id)==ids.end())ids.push_back(r.id);
 double filterUs=0,splineUs=0,filterSq=0,splineSq=0,maxFilter=0,maxSpline=0;
 size_t count=0, splineCount=0, failures=0;
 for(int id:ids){
  auto p=engine::input::InputProcessor::create({25.0,0.12,8.0});
  auto f=engine::stroke::SplineFitter::create();
  if(!p||!f||!p.value().begin_stroke())return 5;
  engine::stroke::IncrementalSplineBuilder builder(std::move(f.value()));
  double last=-1;std::vector<Sample> originals;
  std::vector<engine::input::CleanInputPoint> clean;
  for(auto&r:rows){if(r.id!=id)continue;
   double t=r.ns/1e9;
   if(t<last){failures++;continue;}
    last=t;
   engine::input::RawInputPoint point{{r.x,r.y},t,r.p,r.tx,r.ty};
   auto a=Clock::now();auto result=p.value().process(point);auto b=Clock::now();
   if(!result){failures++;continue;}
   filterUs+=micros(a,b);auto c=result.value();
   double e=std::hypot(c.position.x-r.x,c.position.y-r.y);
   filterSq+=e*e;maxFilter=std::max(maxFilter,e);count++;
   engine::stroke::SplinePoint sp{c.position,c.timestamp,c.pressure,c.tilt_x,c.tilt_y,c.speed};
    a=Clock::now();auto appended=builder.append(sp);b=Clock::now();splineUs+=micros(a,b);
   if(!appended){failures++;continue;}
   originals.push_back(r);clean.push_back(c);
  }
  // Compare spline at original sample timestamps; do not use a resampled curve
  auto a=Clock::now();auto snapshot=builder.snapshot();auto b=Clock::now();
  double snapshotUs=micros(a,b);
  if(!snapshot){std::cerr<<"Spline snapshot failed\n";continue;}
  auto&curve=snapshot.value();
  for(size_t i=0;i<originals.size();++i){
   auto&r=originals[i];auto&c=clean[i];
   auto eval=curve.evaluate_at_time(c.timestamp);
   double sx=std::nan(""),sy=std::nan(""),err=std::nan("");
   if(eval){sx=eval.value().position.x;sy=eval.value().position.y;
     err=std::hypot(sx-r.x,sy-r.y);splineSq+=err*err;maxSpline=std::max(maxSpline,err);splineCount++;}
   out<<r.id<<','<<r.ns<<','<<r.x<<','<<r.y<<','<<c.position.x<<','<<c.position.y<<','
      <<std::hypot(c.position.x-r.x,c.position.y-r.y)<<','<<sx<<','<<sy<<','<<err<<'\n';
  }
  std::cout<<"pointer="<<id<<" samples="<<originals.size()<<" snapshot_us="<<snapshotUs<<"\n";
 }
 std::cout<<"accepted="<<count<<" failures="<<failures
  <<" filter_avg_us="<<(count?filterUs/count:0)
  <<" spline_append_avg_us="<<(count?splineUs/count:0)
  <<" filter_rms_deviation_px="<<(count?std::sqrt(filterSq/count):0)
  <<" filter_max_deviation_px="<<maxFilter
  <<" spline_rms_deviation_px="<<(splineCount?std::sqrt(splineSq/splineCount):0)
  <<" spline_max_deviation_px="<<maxSpline<<"\n";
  std::cout<<"Deviations are relative to raw, not ground truth. Time-only queries select the last sample at a tied instant.\n";
}
