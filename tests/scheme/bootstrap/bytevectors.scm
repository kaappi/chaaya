;; Bytevectors + binary ports (Phase 7A subset)

(check-assert "bytevector? literal" (bytevector? #u8(1 2 3)))
(check-assert "bytevector? false on vector" (not (bytevector? #(1 2 3))))

(check-equal "bytevector constructor"
             #u8(1 2 3)
             (bytevector 1 2 3))
(check-equal "make-bytevector default fill"
             #u8(0 0 0)
             (make-bytevector 3))
(check-equal "make-bytevector custom fill"
             #u8(7 7 7)
             (make-bytevector 3 7))

(check-eqv "bytevector-length" 3 (bytevector-length #u8(4 5 6)))
(check-eqv "bytevector-u8-ref" 5 (bytevector-u8-ref #u8(4 5 6) 1))

(check-equal "bytevector-u8-set!"
             #u8(1 9 3)
             (let ((bv (bytevector 1 2 3)))
               (bytevector-u8-set! bv 1 9)
               bv))

(check-equal "bytevector-copy subset"
             #u8(20 30)
             (bytevector-copy #u8(10 20 30 40) 1 3))

(check-equal "bytevector-copy! subset"
             #u8(0 8 9 0)
             (let ((to (bytevector 0 0 0 0))
                   (from (bytevector 7 8 9)))
               (bytevector-copy! to 1 from 1 3)
               to))

(check-equal "bytevector-append"
             #u8(1 2 3 4)
             (bytevector-append #u8(1 2) #u8(3 4)))

(check-equal "list->bytevector"
             #u8(9 8 7)
             (list->bytevector '(9 8 7)))

(check-equal "bytevector->list"
             '(8 7)
             (bytevector->list #u8(9 8 7) 1 3))

(check-equal "utf8->string ascii"
             "hello"
             (utf8->string #u8(104 101 108 108 111)))

(check-equal "string->utf8 ascii"
             #u8(104 101 108 108 111)
             (string->utf8 "hello"))

(check-equal "read-bytevector zero length"
             #u8()
             (let ((p (open-input-bytevector #u8(1 2 3))))
               (read-bytevector 0 p)))

(check-eqv "read-bytevector zero keeps position"
           1
           (let ((p (open-input-bytevector #u8(1 2 3))))
             (read-bytevector 0 p)
             (read-u8 p)))

(check-eqv "read-bytevector! zero length"
           0
           (let ((p (open-input-bytevector #u8(1 2 3)))
                 (target (make-bytevector 0)))
             (read-bytevector! target p)))

(check-eqv "read-bytevector! zero keeps position"
           1
           (let ((p (open-input-bytevector #u8(1 2 3)))
                 (target (make-bytevector 0)))
             (read-bytevector! target p)
             (read-u8 p)))

(check-eqv "read-u8"
           65
           (read-u8 (open-input-bytevector #u8(65))))

(check-eqv "peek-u8"
           99
           (let ((p (open-input-bytevector #u8(99 100))))
             (peek-u8 p)))

(check-eqv "peek-u8 does not consume"
           99
           (let ((p (open-input-bytevector #u8(99 100))))
             (peek-u8 p)
             (read-u8 p)))

(check-assert "read-u8 eof"
              (eof-object? (read-u8 (open-input-bytevector #u8()))))

(check-equal "open-output-bytevector + get-output-bytevector"
             #u8(65 66 67)
             (let ((p (open-output-bytevector)))
               (write-u8 65 p)
               (write-bytevector #u8(66 67 68) p 0 2)
               (get-output-bytevector p)))

(check-equal "read-bytevector partial"
             #u8(1 2)
             (read-bytevector 2 (open-input-bytevector #u8(1 2 3))))

(check-eqv "read-bytevector! count"
           3
           (let ((target (make-bytevector 4 0))
                 (p (open-input-bytevector #u8(1 2 3))))
             (read-bytevector! target p 1 4)))

(check-equal "read-bytevector! data"
             #u8(0 1 2 3)
             (let ((target (make-bytevector 4 0))
                   (p (open-input-bytevector #u8(1 2 3))))
               (read-bytevector! target p 1 4)
               target))

(check-assert "u8-ready? before read"
              (u8-ready? (open-input-bytevector #u8(1 2 3))))

(check-assert "u8-ready? at eof"
              (let ((p (open-input-bytevector #u8(1))))
                (read-u8 p)
                (u8-ready? p)))

(check-finish)
