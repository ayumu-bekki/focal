# IN のシェーダーを、C++ の生文字列リテラルとして OUT に書く
file(READ ${IN} body)
file(WRITE ${OUT} "R\"MSL(${body})MSL\"\n")
