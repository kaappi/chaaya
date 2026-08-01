; Shared mutable cell via closure
(define (box v)
  (lambda (msg . args)
    (if (eq? msg 'get)
        v
        (if (eq? msg 'set!)
            (begin (set! v (car args)) v)
            #f))))

(define b (box 10))
(b 'set! 20)
(display (b 'get))
(newline)

(define (compose f g)
  (lambda (x) (f (g x))))

(define add1 (lambda (x) (+ x 1)))
(define double (lambda (x) (* x 2)))
(display ((compose add1 double) 10))
(newline)
