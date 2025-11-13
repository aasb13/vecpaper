// https://www.shadertoy.com/view/w3Sfzd

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

mat2 rotate(float a){
    float s = sin(a);
    float c = cos(a);
    return mat2(c,-s,s,c);
}

// Sphere intersection
vec2 sphIntersect(in vec3 ro, in vec3 rd, in vec3 ce, float ra) {
    vec3 oc = ro - ce;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - ra*ra;
    float h = b*b - c;
    if (h<0.0) return vec2(-1.0);
    h = sqrt(h);
    return vec2(-b-h, -b+h);
}

// Camera matrix
mat3 setCamera(in vec3 ro, in vec3 ta, float cr) {
    vec3 cw = normalize(ta - ro);
    vec3 cp = vec3(sin(cr), cos(cr), 0.0);
    vec3 cu = normalize(cross(cw, cp));
    vec3 cv = normalize(cross(cu, cw));
    return mat3(cu, cv, cw);
}

// ACES tonemapping
vec3 Tonemap_ACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return (x*(a*x + b)) / (x*(c*x + d) + e);
}

// Hash without sine
vec3 hash33(vec3 p3) {
    p3 = fract(p3 * vec3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

// Voronoi on the sphere
vec2 voronoi_sphere(vec3 uvw) {
    vec3 cell = floor(uvw);
    float radius = length(uvw);
    float center = 0.5;

    vec2 min_d = vec2(1e4);

    for(int x=-1;x<=1;x++)
    for(int y=-1;y<=1;y++)
    for(int z=-1;z<=1;z++) {
        vec3 by = cell + vec3(x,y,z);
        float sphere = abs(length(by + center) - radius);
        if(sphere < center) {
            vec3 dir = hash33(by + sin(T * 0.0001)) + by;
            float dist = length((dir / length(dir) * radius) - uvw);
            if(dist < min_d.x) { min_d.y = min_d.x; min_d.x = dist; }
            else if(dist < min_d.y) { min_d.y = dist; }
        }
    }
    min_d = max(min_d, 0.001); // avoid zero / NaN
    return min_d;
}

void main() {
    vec2 I = gl_FragCoord.xy;
    vec2 R = resolution.xy;
    vec2 uv = (I * 2.0 - R) / R.y;

    // Mouse rotation scale
    vec2 m = (mouse * 2.0 - R) / R * 6.0;

    vec3 ro = vec3(0.,0.,-10.);
    vec3 rd = setCamera(ro, vec3(0), 0.) * normalize(vec3(uv, 1.)); // fixed camera
    vec3 col = vec3(0);

    vec3 sph_cen = vec3(0);
    vec2 hit = sphIntersect(ro, rd, sph_cen, 6.0);
    if(hit.x > 0.0) {
        vec3 p = ro + rd * hit.x;

        // Rotate points with mouse
        p.xz *= rotate(m.x * 0.3); // scaled down rotation
        p.yz *= rotate(m.y * 0.3);

        vec3 nor = normalize(p - sph_cen);

        vec3 l_dir = normalize(vec3(4,4,-10) - p);
        float diff = max(0., dot(l_dir, nor));
        float spe = pow(max(0., dot(normalize(l_dir - rd), nor)), 30.);

        vec3 obj_col = vec3(0,1,1);
        col += obj_col*0.1 + obj_col*diff*0.3 + spe*0.5;

        vec2 vor = voronoi_sphere(p);
        col += pow(0.2/(vor.y - vor.x), 2.0) * s1(vec3(3,2,1) + p*2.0);
    }

    col = Tonemap_ACES(col);
    gl_FragColor = vec4(col, 1.0);
}
