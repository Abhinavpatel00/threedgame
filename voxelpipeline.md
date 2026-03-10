-skip faces between two voxel 
-if faces with same texture are just forming a kind of plane then combine the triangles(cant do in real time may be)
- so normals of each triangle or quad need not to be stored it can just bit in 3 bits  because only 6 normals are possible in voxel

So your packed normal index looks like this:

0 = +X
1 = -X
2 = +Y
3 = -Y
4 = +Z
5 = -Z
6 = unused
7 = unused
float3 normals[6] =
{
    float3( 1,0,0),
    float3(-1,0,0),
    float3(0, 1,0),
    float3(0,-1,0),
    float3(0,0, 1),
    float3(0,0,-1)
};
Stored as:

[ n2 n1 n0 ]

- about positions its a lil bit complicated beacuse it really depends on chunk size 
for               each coordinate must store a value from 0 → chunkSize-1

0..15
needs 4 bits

x : 4 bits
y : 4 bits
z : 4 bits

Total = 12 bits

0..31
needs 5 bits

Position:

x : 5
y : 5
z : 5

Total = 15 bits

0..127
needs 7 bits

Position:

x : 7
y : 7
z : 7

Total = 21 bits

Now let’s visualize a realistic packed voxel face record. Assume chunk size 128.

| z6 z5 z4 z3 z2 z1 z0 |
| y6 y5 y4 y3 y2 y1 y0 |
| x6 x5 x4 x3 x2 x1 x0 |
| n2 n1 n0 |

uint packed;

bits 0–6   = x
bits 7–13  = y
bits 14–20 = z
bits 21–23 = normal
bits 24–31 = material / texture / flags
uint packed =
    (x) |
    (y << 7) |
    (z << 14) |
    (normal << 21) |
    (material << 24);



- Group voxels into chunks (e.g., 32×32×32).
- Each chunk generates a mesh.
- Render chunks instead of individual voxels.
since there are chunks we can also do frustum culling 

we can generate the world around player and do frustum culling to exclude not visible stuff may be it wont be needed but cant really say anything 

