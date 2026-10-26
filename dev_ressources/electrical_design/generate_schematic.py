#!/usr/bin/env python3
"""Reflow-oven electrical block diagram generator.

One layout, two backends:
  * SVG  -> dev_ressources/electrical_design/schematic_preview.svg  (preview)
  * ODF  -> dev_ressources/electrical_design/electrical_design.odp  (editable in LibreOffice Impress)

Coordinates are in "design px" on a 1240x820 canvas; the ODF backend maps px->cm
at 0.025 cm/px (page = 31.0 x 20.5 cm, landscape).
"""
import html, os, zipfile, xml.dom.minidom

W, H = 1240, 820
C = dict(L="#8a5a2b", N="#1f5fb0", PE="#2e9e3f", SW="#8e44ad", SIG="#c0392b",
         V33="#e08a00", V5="#b8860b", GND="#111111",
         box="#ffffff", boxln="#222222", txt="#111111", band="#f7efe8", bandln="#e3d2c2")
SVGFONT = "DejaVu Sans, Arial, sans-serif"
ODFFONT = "DejaVu Sans"
def esc(s): return html.escape(str(s))

# ---------------------------------------------------------------- SVG backend
class SVG:
    def __init__(self): self.e=[f'<rect x="0" y="0" width="{W}" height="{H}" fill="white"/>']
    def rect(self,x,y,w,h,fill,stroke,sw,rx=0):
        st=f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ''
        self.e.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" ry="{rx}" fill="{fill}"{st}/>')
    def line(self,x1,y1,x2,y2,color,w,dash=None):
        da=f' stroke-dasharray="{dash}"' if dash else ''
        self.e.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{w}" stroke-linecap="round"{da}/>')
    def dot(self,cx,cy,r,color):
        self.e.append(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{color}"/>')
    def label(self,x,y,s,size,anchor='middle',weight='400',fill=None,rot=0):
        tr=f' transform="rotate({rot} {x} {y})"' if rot else ''
        self.e.append(f'<text x="{x}" y="{y}" font-family="{SVGFONT}" font-size="{size}" '
                      f'font-weight="{weight}" text-anchor="{anchor}" fill="{fill or C["txt"]}"{tr}>{esc(s)}</text>')
    def vlabel(self,sx,sy,cx,cy,s,size): self.label(sx,sy,s,size,'middle','600',C['txt'],rot=-90)
    def out(self):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">\n'
                + "\n".join(self.e) + "\n</svg>\n")

# ---------------------------------------------------------------- ODF backend
S = 0.025  # cm per px
def cm(v): return f"{v*S:.3f}cm"
def pt(sz): return f"{sz*S*28.3465:.1f}pt"

class ODF:
    def __init__(self):
        self.body=[]; self._defs=[]; self._map={}; self._n=0
    def _style(self, family, inner):
        key=(family,inner)
        if key in self._map: return self._map[key]
        self._n+=1; name=f"A{self._n}"; self._map[key]=name
        self._defs.append(f'<style:style style:name="{name}" style:family="{family}">{inner}</style:style>')
        return name
    def _g(self, gp): return self._style('graphic', f'<style:graphic-properties {gp}/>')
    def rect(self,x,y,w,h,fill,stroke,sw,rx=0):
        gp=f'draw:fill="solid" draw:fill-color="{fill}"'
        gp+= (f' draw:stroke="solid" svg:stroke-width="{cm(sw)}" svg:stroke-color="{stroke}"' if stroke else ' draw:stroke="none"')
        cr=f' draw:corner-radius="{cm(rx)}"' if rx else ''
        self.body.append(f'<draw:rect draw:style-name="{self._g(gp)}" svg:x="{cm(x)}" svg:y="{cm(y)}" svg:width="{cm(w)}" svg:height="{cm(h)}"{cr}/>')
    def line(self,x1,y1,x2,y2,color,w,dash=None):
        st=self._g(f'draw:stroke="solid" svg:stroke-width="{cm(w)}" svg:stroke-color="{color}" draw:fill="none" svg:stroke-linecap="round"')
        self.body.append(f'<draw:line draw:style-name="{st}" svg:x1="{cm(x1)}" svg:y1="{cm(y1)}" svg:x2="{cm(x2)}" svg:y2="{cm(y2)}"/>')
    def dot(self,cx,cy,r,color):
        st=self._g(f'draw:fill="solid" draw:fill-color="{color}" draw:stroke="none"')
        self.body.append(f'<draw:ellipse draw:style-name="{st}" svg:x="{cm(cx-r)}" svg:y="{cm(cy-r)}" svg:width="{cm(2*r)}" svg:height="{cm(2*r)}"/>')
    def label(self,x,y,s,size,anchor='middle',weight='400',fill=None,rot=0):
        tw=len(s)*size*0.60; FW=tw+10; FH=size*1.3
        if anchor=='middle': fx=x-FW/2; al='center'
        elif anchor=='end':  fx=x-FW;   al='right'
        else:                fx=x;       al='left'
        fy=y-size*0.98
        gst=self._g('draw:fill="none" draw:stroke="none" fo:padding-top="0cm" fo:padding-bottom="0cm" '
                    'fo:padding-left="0cm" fo:padding-right="0cm" draw:auto-grow-width="false" draw:auto-grow-height="false"')
        w='bold' if str(weight) in ('600','700') else 'normal'
        tst=self._style('text', f'<style:text-properties fo:font-size="{pt(size)}" fo:font-weight="{w}" '
                                f'fo:color="{fill or C["txt"]}" style:font-name="{ODFFONT}"/>')
        pst=self._style('paragraph', f'<style:paragraph-properties fo:text-align="{al}"/>')
        self.body.append(
            f'<draw:frame draw:style-name="{gst}" svg:x="{cm(fx)}" svg:y="{cm(fy)}" svg:width="{cm(FW)}" svg:height="{cm(FH)}">'
            f'<draw:text-box><text:p text:style-name="{pst}"><text:span text:style-name="{tst}">{esc(s)}</text:span>'
            f'</text:p></draw:text-box></draw:frame>')
    def vlabel(self,sx,sy,cx,cy,s,size): self.label(cx,cy+size*0.35,s,size,'middle','600',C['txt'])
    def out(self):
        NS=('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
            'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
            'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
            'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
            'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
            'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
            'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0"')
        content=('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<office:document-content {NS} office:version="1.2">'
            f'<office:font-face-decls><style:font-face style:name="{ODFFONT}" svg:font-family="&apos;{ODFFONT}&apos;"/></office:font-face-decls>'
            '<office:automatic-styles>'+''.join(self._defs)+'<style:style style:name="dp1" style:family="drawing-page"/>'
            '</office:automatic-styles>'
            '<office:body><office:presentation>'
            '<draw:page draw:name="Schematic" draw:style-name="dp1" draw:master-page-name="Default">'
            +''.join(self.body)+
            '</draw:page></office:presentation></office:body></office:document-content>')
        styles=('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<office:document-styles {NS} office:version="1.2">'
            '<office:styles/><office:automatic-styles>'
            '<style:page-layout style:name="PM1"><style:page-layout-properties '
            f'fo:page-width="{cm(W)}" fo:page-height="{cm(H)}" fo:margin-top="0cm" fo:margin-bottom="0cm" '
            'fo:margin-left="0cm" fo:margin-right="0cm" style:print-orientation="landscape"/></style:page-layout>'
            '</office:automatic-styles><office:master-styles>'
            '<style:master-page style:name="Default" style:page-layout-name="PM1"/>'
            '</office:master-styles></office:document-styles>')
        manifest=('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.2">'
            '<manifest:file-entry manifest:full-path="/" manifest:version="1.2" manifest:media-type="application/vnd.oasis.opendocument.presentation"/>'
            '<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>'
            '<manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>'
            '</manifest:manifest>')
        return content, styles, manifest

# ------------------------------------------------------------- shared helpers
def box(r,x,y,w,h,title="",sub=None,rx=10,fill=None,tsize=18):
    r.rect(x,y,w,h, fill or C["box"], C["boxln"], 2.2, rx); cy=y+h/2
    if title and sub:
        r.label(x+w/2,cy-3,title,tsize,'middle','600',C["txt"]); r.label(x+w/2,cy+19,sub,14,'middle','400',C["txt"])
    elif title:
        r.label(x+w/2,cy+6,title,tsize,'middle','600',C["txt"])
def wire(r,pts,color,wd=3.0):
    for (x1,y1),(x2,y2) in zip(pts,pts[1:]): r.line(x1,y1,x2,y2,color,wd)
def dot(r,x,y,color,rad=4.2): r.dot(x,y,rad,color)
def cap_v(r,cx,yt,yb,lab_txt):
    mid=(yt+yb)/2
    r.line(cx,yt,cx,mid-7,C["SIG"],2.6); r.line(cx,yb,cx,mid+7,C["SIG"],2.6)
    r.line(cx-13,mid-7,cx+13,mid-7,C["GND"],2.6); r.line(cx-13,mid+7,cx+13,mid+7,C["GND"],2.6)
    r.label(cx,yb+22,lab_txt,13,'middle')
def fuse_h(r,cx,cy,lab_txt=None,lab_dy=-15):
    r.rect(cx-23,cy-8,46,16,'white',C["GND"],2,8); r.line(cx-23,cy,cx+23,cy,C["GND"],1.5)
    if lab_txt: r.label(cx,cy+lab_dy,lab_txt,12,'middle')
def fuse_v(r,cx,cy,lab_txt=None,lab_dx=15):
    r.rect(cx-8,cy-23,16,46,'white',C["GND"],2,8); r.line(cx,cy-23,cx,cy+23,C["GND"],1.5)
    if lab_txt: r.label(cx+lab_dx,cy+4,lab_txt,12,'start')
def switch_h(r,cx,cy):
    r.dot(cx-13,cy,3,C["GND"]); r.dot(cx+13,cy,3,C["GND"]); r.line(cx-13,cy,cx+10,cy-13,C["GND"],2.2)

# --------------------------------------------------------------------- layout
def build(r):
    yband=540
    r.rect(0,yband,W,H-yband,C["band"],None,0)
    r.line(0,yband,W,yband,C["bandln"],2)
    r.label(470,yband+24,"230 V MAINS",14,'start','700',"#9a6a3a")
    r.label(20,36,"Reflow Oven — Electrical Design",22,'start','700')

    yL,yN,yPE=748,770,792
    wire(r,[(85,yL),(1010,yL)],C["L"],3.4); wire(r,[(85,yN),(1060,yN)],C["N"],3.4); wire(r,[(85,yPE),(1110,yPE)],C["PE"],3.4)
    for yy,nm,col in ((yL,"L",C["L"]),(yN,"N",C["N"]),(yPE,"PE",C["PE"])): r.label(74,yy+4,nm,14,'end','700',col)

    ex,ey,ew,eh=50,80,150,380; box(r,ex,ey,ew,eh); r.vlabel(ex+24,ey+eh/2,ex+ew/2,ey+eh/2,"E32R35T (ESP32)",17)
    for nm,yy,col in [("21  CS",120,C["SIG"]),("18  SCK",146,C["SIG"]),("19  MISO",172,C["SIG"]),
                      ("+3.3V",205,C["V33"]),("GND",231,C["GND"]),("32  GPIO",420,C["SIG"])]:
        r.label(ex+ew-8,yy+4,nm,12,'end','600',col)
    r.label(95,ey+eh-8,"+5V",12,'middle','600',C["V5"]); r.label(170,ey+eh-8,"GND",12,'middle','600',C["GND"])

    mx,my,mw,mh=300,95,170,140; box(r,mx,my,mw,mh,"MAX31855")
    tx,ty,tw,th=560,95,200,120; box(r,tx,ty,tw,th,"K-Type","Thermocouple")
    r.label(mx+mw+6,131,"T+",13,'start'); r.label(mx+mw+6,201,"T-",13,'start')

    sx,sy,sw,sh=300,300,170,180; box(r,sx,sy,sw,sh,"Solid-State","Relay")
    r.label(sx+10,349,"GND",12,'start','400',C["GND"]); r.label(sx+10,424,"IN",12,'start','400',C["SIG"])
    r.label(455,sy+sh-12,"L~",12,'middle','400',C["L"])

    h2x,h2y,h2w,h2h=560,300,170,60; box(r,h2x,h2y,h2w,h2h,"Heater 2","(750 W)")
    h1x,h1y,h1w,h1h=560,388,170,60; box(r,h1x,h1y,h1w,h1h,"Heater 1","(750 W)")
    fx,fy,fw,fh=800,300,140,60; box(r,fx,fy,fw,fh,"Ventilator")
    lx,ly,lw,lh=800,388,140,60; box(r,lx,ly,lw,lh,"Lamp")

    px,py,pw,ph=50,600,200,110; box(r,px,py,pw,ph,"AC/DC","230 V → +5 V")
    ix,iy,iw,ih=960,560,220,160; box(r,ix,iy,iw,ih); r.label(ix+iw/2,iy+24,"IEC C14 inlet",15,'middle','600')
    hox,hoy,how,hoh=300,600,140,60; box(r,hox,hoy,how,hoh,"Housing")

    # wiring
    for yy in (120,146,172): wire(r,[(ex+ew,yy),(mx,yy)],C["SIG"],2.6)
    wire(r,[(ex+ew,205),(mx,205)],C["V33"],2.8); wire(r,[(ex+ew,231),(mx,231)],C["GND"],2.8)
    wire(r,[(mx+mw,131),(tx,131)],C["SIG"],2.8); wire(r,[(mx+mw,201),(tx,201)],C["SIG"],2.8)
    cap_v(r,515,131,201,"10 nF"); dot(r,515,131,C["SIG"]); dot(r,515,201,C["SIG"])

    rx=250
    wire(r,[(rx,231),(rx,345)],C["GND"],2.6); dot(r,rx,231,C["GND"])   # GND bus down (aligned w/ R)
    wire(r,[(rx,345),(sx,345)],C["GND"],2.6); dot(r,rx,345,C["GND"])   # GND across to SSR GND pin
    r.rect(rx-8,367,16,30,'white',C["GND"],2)                         # 10k (centered in the gap)
    wire(r,[(rx,345),(rx,367)],C["GND"],2.4)                          # R top lead (GND)
    wire(r,[(rx,397),(rx,420)],C["SIG"],2.4); dot(r,rx,420,C["SIG"])  # R bottom lead -> GPIO line
    r.label(238,388,"10 kΩ",12,'end')
    wire(r,[(ex+ew,420),(sx,420)],C["SIG"],2.8)                       # GPIO32 -> SSR IN

    wire(r,[(sx+sw,375),(545,375)],C["SW"],2.8); dot(r,545,375,C["SW"])
    wire(r,[(545,330),(545,418)],C["SW"],2.8)
    wire(r,[(545,330),(h2x,330)],C["SW"],2.8); wire(r,[(545,418),(h1x,418)],C["SW"],2.8)
    wire(r,[(455,sy+sh),(455,yL)],C["L"],3.0); dot(r,455,yL,C["L"])

    # Heater neutral return: both heaters join at x=755, then the thermal fuse
    # sits in series in that shared N leg before it reaches the N rail.
    wire(r,[(755,330),(755,477)],C["N"],3.0)
    fuse_v(r,755,500)
    r.label(738,494,"Thermal",12,'end'); r.label(738,510,"Fuse",12,'end')
    wire(r,[(755,523),(755,yN)],C["N"],3.0); dot(r,755,yN,C["N"])
    wire(r,[(h2x+h2w,330),(755,330)],C["N"],3.0)                       # corner (no junction dot)
    wire(r,[(h1x+h1w,418),(755,418)],C["N"],3.0); dot(r,755,418,C["N"])

    wire(r,[(785,330),(785,yL)],C["L"],2.6); dot(r,785,yL,C["L"])
    wire(r,[(950,330),(950,yN)],C["N"],2.6); dot(r,950,yN,C["N"])
    wire(r,[(fx,330),(785,330)],C["L"],2.6)                            # corner (no junction dot)
    wire(r,[(lx,418),(785,418)],C["L"],2.6); dot(r,785,418,C["L"])
    wire(r,[(fx+fw,330),(950,330)],C["N"],2.6)                         # corner (no junction dot)
    wire(r,[(lx+lw,418),(950,418)],C["N"],2.6); dot(r,950,418,C["N"])

    wire(r,[(px+45,py+ph),(px+45,yL)],C["L"],3.0); dot(r,px+45,yL,C["L"])
    wire(r,[(px+120,py+ph),(px+120,yN)],C["N"],3.0); dot(r,px+120,yN,C["N"])
    wire(r,[(px+45,py),(px+45,ey+eh)],C["V5"],2.8)
    wire(r,[(px+120,py),(px+120,ey+eh)],C["GND"],2.8)                 # GND up to ESP32 bottom edge
    r.line(px+160,py+ph,px+160,yPE,C["PE"],2.4,dash="6 5"); dot(r,px+160,yPE,C["PE"])
    r.label(px+168,py+ph+30,"PE–0V bond",11,'start','400',C["PE"])

    wire(r,[(hox+how/2,hoy+hoh),(hox+how/2,yPE)],C["PE"],3.0); dot(r,hox+how/2,yPE,C["PE"])

    wL,wN,wPE=iy+60,iy+92,iy+120; oL,oN,oPE=1000,1050,1100
    for ry,col,nm in ((wL,C["L"],"L"),(wN,C["N"],"N"),(wPE,C["PE"],"PE")):
        wire(r,[(ix+iw,ry),(ix+iw+30,ry)],col,3.0); r.label(ix+iw+36,ry+4,nm,12,'start','400',col)
    wire(r,[(ix+iw,wL),(oL,wL),(oL,yL)],C["L"],2.6); dot(r,oL,yL,C["L"])
    fuse_h(r,1118,wL,"Fuse 8 A",lab_dy=22); switch_h(r,1062,wL)
    wire(r,[(ix+iw,wN),(oN,wN),(oN,yN)],C["N"],2.6); dot(r,oN,yN,C["N"]); switch_h(r,1062,wN)
    wire(r,[(ix+iw,wPE),(oPE,wPE),(oPE,yPE)],C["PE"],2.6); dot(r,oPE,yPE,C["PE"])
    r.line(1062,wL-14,1062,wN+4,C["GND"],1.4,dash="3 3")

    lgx,lgy=985,120
    leg=[("L (live)",C["L"]),("N (neutral)",C["N"]),("PE (earth)",C["PE"]),("SSR → heater",C["SW"]),
         ("Signal / SPI",C["SIG"]),("+3.3 V",C["V33"]),("+5 V",C["V5"]),("GND / 0 V",C["GND"])]
    box(r,lgx-14,lgy-28,200,40+24*len(leg),fill="#fbfbfb",rx=8)
    r.label(lgx,lgy-8,"Legend",14,'start','700')
    for i,(nm,col) in enumerate(leg):
        yy=lgy+22+i*24; r.line(lgx,yy,lgx+30,yy,col,3.6); r.label(lgx+40,yy+4,nm,12,'start')

# --------------------------------------------------------------------- output
HERE=os.path.dirname(os.path.abspath(__file__))
def write_svg(path):
    r=SVG(); build(r); open(path,"w").write(r.out()); return path
def write_odp(path):
    r=ODF(); build(r); content,styles,manifest=r.out()
    xml.dom.minidom.parseString(content); xml.dom.minidom.parseString(styles); xml.dom.minidom.parseString(manifest)
    with zipfile.ZipFile(path,"w") as z:
        zi=zipfile.ZipInfo("mimetype"); zi.compress_type=zipfile.ZIP_STORED
        z.writestr(zi,"application/vnd.oasis.opendocument.presentation")
        z.writestr("META-INF/manifest.xml",manifest,compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("content.xml",content,compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("styles.xml",styles,compress_type=zipfile.ZIP_DEFLATED)
    return path

if __name__=="__main__":
    import sys
    outdir=sys.argv[1] if len(sys.argv)>1 else HERE
    print("svg:", write_svg(os.path.join(outdir,"schematic_preview.svg")))
    print("odp:", write_odp(os.path.join(outdir,"electrical_design.odp")))
