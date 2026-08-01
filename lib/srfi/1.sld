(define-library (srfi 1)
  (import (scheme base))
  (export first second third iota fold filter)
  (begin
    ;; Phase 9 partial shim: provide a small, commonly-used subset.
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
          (else (loop (cdr xs) acc)))))))
