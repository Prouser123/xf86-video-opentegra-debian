.exports
	[0] = "position";
	[1] = "src_texcoords";
	[2] = "mask_texcoords";

.attributes
	[0] = "position";
	[1] = "src_texcoords";
	[2] = "mask_texcoords";

.constants
	[0].z = 0.0;
	[0].w = 1.0;

.asm
EXEC(export[0]=vector)
	MOVv r63.xy**, a[0].xyzw
;

EXEC(export[0]=vector)
	MOVv r63.**zw, c[0].xyzw
;

EXEC(export[1]=vector)
	MOVv r63.xy**, a[1].xyzw
;

EXEC_END(export[1]=vector)
	MOVv r63.**zw, a[2].zwxy
;
