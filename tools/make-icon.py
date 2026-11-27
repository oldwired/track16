# Draws resources/app-icon.png (512 x 512): the pattern rain, dimmed, with a bold 16 over it.
#   python3 tools/make-icon.py resources/app-icon.png      (needs Pillow: pip install pillow)
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from pathlib import Path
F=str(Path(__file__).resolve().parent.parent/'resources/fonts')+'/'
bold=lambda s: ImageFont.truetype(F+'JetBrainsMonoNL-Bold.subset.ttf', s)
med=lambda s: ImageFont.truetype(F+'JetBrainsMonoNL-Medium.subset.ttf', s)
N=512; out=sys.argv[1]
BG=(14,18,16); BAND=(24,52,34); RAIN=(60,220,120); NOTE=(228,255,236)
def rain(dim):
    im=Image.new('RGBA',(N,N),BG+(255,)); d=ImageDraw.Draw(im)
    d.rectangle([0,318,N,382],fill=BAND); d.line([0,316,N,316],fill=RAIN,width=4); d.line([0,384,N,384],fill=RAIN,width=4)
    cols=[(64,['C-5','...','E-5','...','G-5']),(200,['...','A-4','...','C-6','...']),(336,['F-5','...','...','D-5','...'])]
    for x,notes in cols:
        for i,t in enumerate(notes):
            y=40+i*64 if i<4 else 324
            a=int((255 if i>=2 else 120+60*i)*dim)
            col=RAIN if t!='...' else (52,70,60)
            d.text((x,y),t,font=bold(50) if t!='...' else med(50),fill=col+(a,))
    d.text((64,410),'C-5',font=bold(50),fill=(120,150,130,int(140*dim)))
    return im
def sixteen(im, glow):
    # a dark halo behind the digits so they stay clear of the rain, then the digits
    halo=Image.new('L',(N,N),0); ImageDraw.Draw(halo).text((256,236),'16',font=bold(330),fill=255,anchor='mm')
    halo=halo.filter(ImageFilter.MaxFilter(25)).filter(ImageFilter.GaussianBlur(10))
    im.paste(Image.new('RGBA',(N,N),BG+(235,)),(0,0),halo)
    if glow:
        g=Image.new('L',(N,N),0); ImageDraw.Draw(g).text((256,236),'16',font=bold(330),fill=200,anchor='mm')
        g=g.filter(ImageFilter.GaussianBlur(14)); im.paste(Image.new('RGBA',(N,N),RAIN+(255,)),(0,0),g)
    ImageDraw.Draw(im).text((256,236),'16',font=bold(330),fill=NOTE+(255,),anchor='mm')
    return im
def finish(im,name):
    m=Image.new('L',(N,N),0); ImageDraw.Draw(m).rounded_rectangle([0,0,N-1,N-1],radius=104,fill=255)
    o=Image.new('RGBA',(N,N),(0,0,0,0)); o.paste(im,(0,0),m)
    ImageDraw.Draw(o).rounded_rectangle([2,2,N-3,N-3],radius=102,outline=(60,220,120,90),width=5) # a thin edge on dark menus
    o.save(out); return o
im=finish(sixteen(rain(0.55),False),'icon')
