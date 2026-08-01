; Tail-recursive countdown
(define (countdown n)
  (if (= n 0)
      0
      (countdown (- n 1))))

(display (countdown 1000))
(newline)

(define (fib n)
  (define (iter a b k)
    (if (= k 0)
        a
        (iter b (+ a b) (- k 1))))
  (iter 0 1 n))

(display (fib 10))
(newline)
