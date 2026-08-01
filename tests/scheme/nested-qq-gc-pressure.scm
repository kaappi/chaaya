;; Force GC pressure during compile of nested quasiquote (`,,` / `,'`)
(import (scheme base))

;; Allocate many strings before compiling the quasiquote form below
(define junk
  (let loop ((n 0) (acc '()))
    (if (= n 50000)
        acc
        (loop (+ n 1) (cons (make-string 32 #\x) acc)))))

(define (check msg got expected)
  (unless (equal? got expected)
    (error "FAIL" msg got expected)))

(let ((name1 'x)
      (name2 'y))
  (check "double-unquote + quote-unquote"
    '(a `(b ,x ,'y d) e)
    `(a `(b ,,name1 ,',name2 d) e)))

(display "OK\n")
