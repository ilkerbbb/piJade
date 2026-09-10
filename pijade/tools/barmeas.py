import sys, struct
W,H=320,200
def bar(path):
    raw=open(path,'rb').read()
    # progress bar row: lower part, around y=180
    counts=[]
    for y in (176,178,180,182):
        n=0
        for x in range(10,310):
            p=struct.unpack_from('<H', raw, 2*(y*W+x))[0]
            r=(p>>11)&0x1f; g=(p>>5)&0x3f; b=p&0x1f
            # jade green: g dominant, r and b low
            if g>15 and r<12 and b<12: n+=1
        counts.append(n)
    return max(counts)
for p in sys.argv[1:]:
    print(p, bar(p))
