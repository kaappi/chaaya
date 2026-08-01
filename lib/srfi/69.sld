(define-library (srfi 69)
  (import (scheme base))
  (export make-hash-table
          hash-table?
          hash-table-ref
          hash-table-set!
          hash-table-delete!
          hash-table-size
          hash-table-keys
          hash-table-values
          hash-table-walk
          hash-table-fold
          hash-table-ref/default
          hash-table-exists?
          hash-table-update!
          hash-table-update!/default
          alist->hash-table
          hash-table->alist)
  (begin
    ;; Phase 9 partial shim: extend core hash-table primitives with SRFI names.
    (define (hash-table-ref/default ht key default)
      (hash-table-ref ht key default))

    (define (hash-table-exists? ht key)
      (let ((missing (cons #f #f)))
        (not (eq? (hash-table-ref ht key missing) missing))))

    (define (hash-table-update! ht key proc)
      (hash-table-set! ht key (proc (hash-table-ref ht key)))
      ht)

    (define (hash-table-update!/default ht key proc default)
      (hash-table-set! ht key (proc (hash-table-ref ht key default)))
      ht)

    (define (alist->hash-table alist . opt)
      (let ((ht (if (pair? opt) (make-hash-table (car opt)) (make-hash-table))))
        (for-each (lambda (entry)
                    (hash-table-set! ht (car entry) (cdr entry)))
                  alist)
        ht))

    (define (hash-table->alist ht)
      (hash-table-fold ht
                       (lambda (k v acc) (cons (cons k v) acc))
                       '()))))
