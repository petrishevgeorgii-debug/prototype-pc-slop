#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A textured software picture of Pure3D world geometry (no game code, no GPU): a check of
meshes, shaders and textures together. Slow (Python), for a cell or two.

  python tools/formats/render_textured.py out.png manhattan_Cell_100.p3d.rz [textures.p3d.rz ...]
"""
import argparse
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pure3d  # noqa: E402

COLOUR_PARAMS = ('color', 'colour', 'diffuse', 'color_inards', 'color_infection')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out')
    p.add_argument('files', nargs='+')
    args = p.parse_args()
    roots = [pure3d.load(f) for f in args.files]
    texc, shaders = {}, {}
    for root in roots:
        texc.update(pure3d.textures(root))
        for _, c in root.walk():
            if c.id == 0x11015:
                n, _, t = pure3d.shader(c)
                shaders[n] = t
    cache={}
    def tex(name):
        if name not in cache:
            img=None
            if name in texc:
                r=pure3d.texture(texc[name])[3]
                if not isinstance(r,str): img=np.asarray(r.convert('RGB'),np.float32)/255
            cache[name]=img
        return cache[name]
    W,H=1600,1000
    items=[]
    for mesh,prims in [m for root in roots for m in pure3d.meshes(root)]:
        for p in prims:
            pos=pure3d.positions(p)
            if not pos: continue
            v=np.asarray(pos,np.float32)
            if np.linalg.norm(v.mean(0))<60 and np.ptp(v,0).max()<60: continue
            uvel=next((e for e in p.elements if e[0]=='uv0'),None)
            n=len(pos)
            uv=np.array([struct.unpack_from('<2f',p.vertices,k*p.stride+uvel[1]) for k in range(n)],np.float32) if uvel else np.zeros((n,2),np.float32)
            t=shaders.get(p.shader,{})
            tn=next((t[k] for k in COLOUR_PARAMS if k in t),None)
            idx=np.asarray(p.indices[:len(p.indices)//3*3]).reshape(-1,3)
            items.append((v,uv,idx,tex(tn) if tn else None))
    allv=np.concatenate([i[0] for i in items])
    lo,hi=np.percentile(allv,2,0),np.percentile(allv,98,0); centre=(lo+hi)/2; ext=max(hi[0]-lo[0],hi[2]-lo[2])
    eye=centre+np.array([-0.32,0.22,-0.42])*ext
    fwd=centre-eye; fwd/=np.linalg.norm(fwd); right=np.cross(fwd,[0,1,0]); right/=np.linalg.norm(right); up=np.cross(right,fwd)
    f=W/(2*math.tan(math.radians(32)))
    img=np.zeros((H,W,3),np.float32); img[:]=(0.55,0.62,0.7)
    zb=np.full((H,W),np.inf,np.float32)
    sun=np.array([0.4,0.8,0.3]); sun/=np.linalg.norm(sun)
    for v,uv,idx,texture in items:
        rel=v-eye; x=rel@right; y=rel@up; z=rel@fwd
        sx=W/2+x/np.maximum(z,1e-3)*f; sy=H/2-y/np.maximum(z,1e-3)*f
        tri=v[idx]; nrm=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]); nrm/=np.linalg.norm(nrm,axis=1,keepdims=True)+1e-9
        shade=0.45+0.55*np.abs(nrm@sun)
        for t,(a,b,c) in enumerate(idx):
            if z[a]<1 or z[b]<1 or z[c]<1: continue
            xs=np.array([sx[a],sx[b],sx[c]]); ys=np.array([sy[a],sy[b],sy[c]])
            x0,x1=int(max(0,np.floor(xs.min()))),int(min(W-1,np.ceil(xs.max()))); y0,y1=int(max(0,np.floor(ys.min()))),int(min(H-1,np.ceil(ys.max())))
            if x0>x1 or y0>y1: continue
            if (x1-x0)*(y1-y0)>400000: continue
            px,py=np.meshgrid(np.arange(x0,x1+1)+0.5,np.arange(y0,y1+1)+0.5)
            d=(ys[1]-ys[2])*(xs[0]-xs[2])+(xs[2]-xs[1])*(ys[0]-ys[2])
            if abs(d)<1e-9: continue
            w0=((ys[1]-ys[2])*(px-xs[2])+(xs[2]-xs[1])*(py-ys[2]))/d
            w1=((ys[2]-ys[0])*(px-xs[2])+(xs[0]-xs[2])*(py-ys[2]))/d
            w2=1-w0-w1
            m=(w0>=0)&(w1>=0)&(w2>=0)
            if not m.any(): continue
            iz=w0/z[a]+w1/z[b]+w2/z[c]; depth=1/iz
            sub=zb[y0:y1+1,x0:x1+1]; m&=depth<sub
            if not m.any(): continue
            sub[m]=depth[m]
            if texture is not None:
                u=(w0*uv[a,0]/z[a]+w1*uv[b,0]/z[b]+w2*uv[c,0]/z[c])/iz
                vv=(w0*uv[a,1]/z[a]+w1*uv[b,1]/z[b]+w2*uv[c,1]/z[c])/iz
                th,tw=texture.shape[:2]
                col=texture[(np.floor(vv[m]*th).astype(int))%th,(np.floor(u[m]*tw).astype(int))%tw]
            else:
                col=np.array([0.6,0.58,0.55])
            img[y0:y1+1,x0:x1+1][m]=col*shade[t]
    Image.fromarray((np.clip(img,0,1)*255).astype(np.uint8)).save(args.out)
    print(f'{args.out}: {len(items)} parts')


if __name__ == '__main__':
    main()
