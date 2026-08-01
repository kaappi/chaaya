(define-library (srfi 1)
  (import (scheme base))
  (export first second third iota fold filter
          any find lset= lset-intersection lset-difference)
  (begin
    ;; Phase 9 partial shim: commonly-used SRFI-1 subset for portable libraries.
    (define (first xs) (car xs))
    (define (second xs) (car (cdr xs)))
    (define (third xs) (car (cdr (cdr xs))))

    (define (iota count . opt)
      (if (not (and (integer? count) (>= count 0)))
          (error "iota: expected non-negative integer count" count))
      (let ((start (if (pair? opt) (car opt) 0))
            (step (if (and (pair? opt) (pair? (cdr opt))) (cadr opt) 1)))
        (let loop ((n count) (value start) (acc '()))
          (if (= n 0)
              (reverse acc)
              (loop (- n 1) (+ value step) (cons value acc))))))

    (define (fold kons knil lis)
      (let loop ((xs lis) (acc knil))
        (if (pair? xs)
            (loop (cdr xs) (kons (car xs) acc))
            acc)))

    (define (filter pred lis)
      (let loop ((xs lis) (acc '()))
        (cond
          ((null? xs) (reverse acc))
          ((pred (car xs)) (loop (cdr xs) (cons (car xs) acc)))
          (else (loop (cdr xs) acc)))))

    (define (any pred lis)
      (if (null? lis)
          #f
          (or (pred (car lis)) (any pred (cdr lis)))))

    (define (find pred lis)
      (cond
        ((null? lis) #f)
        ((pred (car lis)) (car lis))
        (else (find pred (cdr lis)))))

    (define (lset-member? = elem lis)
      (any (lambda (x) (= elem x)) lis))

    (define (lset-intersection = lis1 . lists)
      (if (null? lists)
          lis1
          (filter (lambda (x)
                    (let loop ((ls lists))
                      (if (null? ls)
                          #t
                          (and (lset-member? = x (car ls))
                               (loop (cdr ls))))))
                  lis1)))

    (define (lset-difference = lis1 . lists)
      (if (null? lists)
          lis1
          (filter (lambda (x)
                    (not (any (lambda (l) (lset-member? = x l)) lists)))
                  lis1)))

    (define (lset= = lis1 . rest)
      (let loop ((lists (cons lis1 rest)))
        (cond
          ((null? (cdr lists)) #t)
          ((not (= (length (car lists)) (length (cadr lists)))) #f)
          ((not (null? (lset-difference = (car lists) (cadr lists)))) #f)
          (else (loop (cdr lists)))))))))
