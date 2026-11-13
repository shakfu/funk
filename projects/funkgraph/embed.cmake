# Writes OUT, a C file defining NAME and NAME_size from IN's bytes.
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "/* ${IN}, by embed.cmake */\n#include <stddef.h>\nconst unsigned char ${NAME}[] = {${bytes}};\nconst size_t ${NAME}_size = ${size};\n")
