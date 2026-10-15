; FLUSH.X - DOS _FLUSH($FF0D)を呼ぶだけ(Human68k のセクタバッファが無効化されるかの実験)
                dc.w    $FF0D           ; DOS _FLUSH
                clr.w   -(sp)
                dc.w    $FF4C           ; DOS _EXIT2(終了コード 0)
