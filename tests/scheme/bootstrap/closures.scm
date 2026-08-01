;; Closures / recursion — patterns from Kaappi smoke and R7RS 4.1.4.

(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))
(check-eqv "fact 5" 120 (fact 5))
(check-eqv "fact 0" 1 (fact 0))

(define (make-adder n)
  (lambda (x) (+ x n)))
(check-eqv "closure adder" 42 ((make-adder 40) 2))

(define (make-counter)
  (let ((n 0))
    (lambda ()
      (set! n (+ n 1))
      n)))
(define c (make-counter))
(check-eqv "counter 1" 1 (c))
(check-eqv "counter 2" 2 (c))
(check-eqv "counter 3" 3 (c))

(define (even? n)
  (if (= n 0) #t (odd? (- n 1))))
(define (odd? n)
  (if (= n 0) #f (even? (- n 1))))
(check-assert "even? 10" (even? 10))
(check-assert "odd? 7" (odd? 7))
(check-assert "not odd? 8" (not (odd? 8)))

(define (loop n acc)
  (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(check-eqv "tail loop" 200 (loop 200 0))

(check-eqv "variadic lambda" 1 ((lambda xs (car xs)) 1 2 3))
(check-eqv "rest lambda" 2 ((lambda (a . rest) (car rest)) 1 2 3))

(check-assert "procedure? lambda" (procedure? (lambda (x) x)))
(check-assert "procedure? +" (procedure? +))
(check-assert "not procedure? 1" (not (procedure? 1)))

(check-finish)
