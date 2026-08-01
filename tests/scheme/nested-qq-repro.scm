;; Minimal repro for nested quasiquote segfault (r7rs-tests.scm:354-356)
(import (scheme base))

(define (check msg got expected)
  (unless (equal? got expected)
    (error "FAIL" msg got expected)))

;; Case A: literal expected form (sanity)
(check "literal"
  '(a `(b ,x ,'y d) e)
  '(a `(b ,x ,'y d) e))

;; Case B: static nested quasiquote (no let)
(check "static"
  '(a `(b ,x ,'y d) e)
  `(a `(b ,,(quote x) ,',(quote y) d) e))

;; Case C: with let bindings (r7rs exact)
(let ((name1 'x)
      (name2 'y))
  (check "let-bound"
    '(a `(b ,x ,'y d) e)
    `(a `(b ,,name1 ,',name2 d) e)))

(display "OK\n")
