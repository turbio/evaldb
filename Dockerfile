#FROM ubuntu:18.04

#RUN apt-get install -y ccache
#RUN apt-get install -y git
#RUN apt-get install -y curl
#RUN apt-get install -y g++
#RUN apt-get install -y make
#RUN apt-get install -y libboost-all-dev
#
#RUN mkdir -p /app
#WORKDIR /app
#ADD . .
#
#RUN mkdir libv8gem && \
#	cd libv8gem && \
#	curl https://rubygems.org/downloads/libv8-6.7.288.46.1-x86_64-linux.gem | tar xv && \
#	tar xzvf data.tar.gz && \
#	cd .. && \
#	ln -s $PWD/libv8gem/vendor/v8/out.gn/libv8/obj lib && \
#	ln -s $PWD/libv8gem/vendor/v8/include include
#

FROM golang:1.11

RUN apt-get update
RUN apt-get install -y libjansson-dev

RUN mkdir -p /go/src/github.com/turbio/evaldb

WORKDIR /go/src/github.com/turbio/evaldb

COPY . .

RUN make

EXPOSE 5000

CMD ./evalserver
