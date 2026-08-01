;; r7rs §4.2 quasiquote slice — triggers ASAN UAF in full suite context
(import (scheme base))

(define (test expected expr)
  (let ((res expr))
    (unless (equal? res expected)
      (error 'fail expected res))))

(test '(list 3 4) `(list ,(+ 1 2) 4))
(let ((name 'a)) (test '(list a (quote a)) `(list ,name ',name)))
(test '(a 3 4 5 6 b) `(a ,(+ 1 2) ,@(map abs '(4 -5 6)) b))

(test '(a `(b ,(+ 1 2) e) f)
  `(a `(b ,(+ 1 2) e) f))

(let ((name1 'x)
      (name2 'y))
  (test '(a `(b ,x ,'y d) e)
        `(a `(b ,,name1 ,',name2 d) e)))

(display "quasiquote slice OK\n")
