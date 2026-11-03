DOCNAME = Campus26_Gruppe3_Designdokument
MAIN = start
TEXFILES = $(wildcard *.tex)
IMAGES = $(wildcard images/*)

all: $(DOCNAME).pdf

$(DOCNAME).pdf: $(TEXFILES) $(IMAGES)
	pdflatex -interaction=nonstopmode -halt-on-error -jobname=$(DOCNAME) $(MAIN).tex
	pdflatex -interaction=nonstopmode -halt-on-error -jobname=$(DOCNAME) $(MAIN).tex

clean:
	rm -f *.aux *.log *.out *.toc *.lof *.lot *.fls *.fdb_latexmk *.synctex.gz
	rm -f $(DOCNAME).pdf
