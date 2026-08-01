(import (scheme base) (scheme write) (chibi test))

(test-begin "4.2 qq")

(define (f n) (number->string n 10))
(define (square x) (* x x))
(define (foo a d) (list a d))

(test '(list 3 4) `(list ,(+ 1 2) 4))
(let ((name 'a)) (test '(list a (quote a)) `(list ,name ',name)))
(test '(a 3 4 5 6 b) `(a ,(+ 1 2) ,@(map abs '(4 -5 6)) b))
(test #(10 5 4 16 9 8)
    `#(10 5 ,(square 2) ,@(map square '(4 3)) 8))
(test '(a `(b ,(+ 1 2) ,(foo 4 d) e) f)
    `(a `(b ,(+ 1 2) ,(foo ,(+ 1 3) d) e) f) )
(let ((name1 'x)
      (name2 'y))
   (test '(a `(b ,x ,'y d) e) `(a `(b ,,name1 ,',name2 d) e)))

(test-end "4.2 qq")
