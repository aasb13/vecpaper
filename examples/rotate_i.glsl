// https://www.shadertoy.com/view/W3XBD7

#version 330 core
uniform vec2 center;
uniform vec2 resolution;
uniform float time;
uniform vec2 mouse;
uniform float pulse1;
uniform float pulse2;
uniform float pulse3;

#define T time
#define PI 3.141596
#define TAU 6.283185
#define S smoothstep
#define s1(v) (sin(v)*.5+.5)
const float EPSILON = 1e-3;

mat2 rotate(float a){
  float s = sin(a);
  float c = cos(a);
  return mat2(c,-s,s,c);
}

float fbm(vec3 p){
  float amp = 1.;
  float fre = 1.;
  float n = 0.;
  for(float i =0.;i<4.;i++){
    n += abs(dot(cos(p), vec3(.1)));
    amp *= .5;
    fre *= 2.;
  }
  return n;
}

void main(){
    vec4 O = vec4(0,0,0,1);
    vec2 I = gl_FragCoord.xy;

  vec2 R = resolution.xy;
  vec2 uv = (I*2.-R)/R.y;
  vec2 m = (mouse.xy*2.-R)/R * PI * 2.;

  O.rgb *= 0.;
  O.a = 1.;

  vec3 ro = vec3(0.,0.,-7.);

  vec3 rd = normalize(vec3(uv, 1.));

  float zMax = 50.;
  float z = .1;

  vec3 col = vec3(0);
  for(float i=0.;i<100.;i++){
    vec3 p = ro + rd * z;

    p.xz = rotate(T+i*2.3)*p.xz;
    p.yz = rotate(T+i*2.3)*p.yz;

    float d = length(p) - 4.;
    d = abs(d)*.6 + .01;

    d += fbm(p*1.8)*.2;

    col += (1.1+sin(vec3(3,2,1)+dot(p,vec3(1.))+T))/d;
    
    if(d<EPSILON || z>zMax) break;
    z += d;
  }

  col = tanh(col / 2e2);

  O.rgb = col;

  gl_FragColor = O;
}