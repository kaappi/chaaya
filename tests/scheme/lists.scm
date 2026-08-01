; Nested lets, closures, and list processing
(define (map-add1 xs)
  (if (null? xs)
      '()
      (cons (+ (car xs) 1) (map-add1 (cdr xs)))))

(define (foldl f acc xs)
  (if (null? xs)
      acc
      (foldl f (f acc (car xs)) (cdr xs))))

(define nums (list 1 2 3 4 5))
(define mapped (map-add1 nums))
(define sum (foldl (lambda (a b) (+ a b)) 0 nums))

(display sum)
(newline)
(display (car mapped))
(newline)
(display (car (cdr (cdr mapped))))
(newline)
