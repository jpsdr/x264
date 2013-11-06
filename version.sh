#!/usr/bin/env bash
[ -n "$1" ] && cd $1
git rev-list HEAD | sort > config.git-hash
LOCAL_VER=`wc -l config.git-hash | awk '{print $1}'`
GIT_HEAD=`git branch --list | grep "*" | awk '{print $2}'`
BIT_DEPTH=`grep "X264_BIT_DEPTH" < x264_config.h | awk '{print $3}'`
CHROMA_FORMATS=`grep "X264_CHROMA_FORMAT" < x264_config.h | awk '{print $3}'`
if [ $CHROMA_FORMATS == "0" ] ; then
    CHROMA_FORMATS="all"
elif [ $CHROMA_FORMATS == "X264_CSP_I420" ] ; then
    CHROMA_FORMATS="4:2:0"
elif [ $CHROMA_FORMATS == "X264_CSP_I422" ] ; then
    CHROMA_FORMATS="4:2:2"
elif [ $CHROMA_FORMATS == "X264_CSP_I444" ] ; then
    CHROMA_FORMATS="4:4:4"
fi
BUILD_ARCH=`grep "ARCH=" < config.mak | awk -F= '{print $2}'`
if [ $LOCAL_VER \> 1 ] ; then
    PLAIN_VER=`git rev-list porigin/master | sort | join config.git-hash - | wc -l | awk '{print $1}'`
    echo "#define X264_REV $PLAIN_VER"
    if [ $PLAIN_VER == $LOCAL_VER ] ; then
        VER=$PLAIN_VER
    else
        VER_DIFF=$(($LOCAL_VER-$PLAIN_VER))
        VER="$PLAIN_VER+$VER_DIFF"
    fi
    echo "#define X264_REV_DIFF $VER_DIFF"
    if git status | grep -q "modified:" ; then
        VER="${VER}M"
    fi
    VER="$VER $(git rev-list HEAD -n 1 | cut -c 1-7) $GIT_HEAD [${BIT_DEPTH}-bit@${CHROMA_FORMATS} ${BUILD_ARCH}]"
    echo "#define X264_VERSION \" r$VER\""
else
    echo "#define X264_VERSION \"\""
    VER="x [${BIT_DEPTH}-bit@${CHROMA_FORMATS} ${BUILD_ARCH}]"
fi
rm -f config.git-hash
API=`grep '#define X264_BUILD' < x264.h | sed -e 's/.* \([1-9][0-9]*\).*/\1/'`
echo "#define X264_POINTVER \"0.$API.$VER\""
