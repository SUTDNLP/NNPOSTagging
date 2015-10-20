/*
 * SparseRNNCRFMLClassifier.h
 *
 *  Created on: Mar 18, 2015
 *      Author: mszhang
 */

#ifndef SRC_SparseRNNCRFMLClassifier_H_
#define SRC_SparseRNNCRFMLClassifier_H_

#include <iostream>

#include <assert.h>
#include "Example.h"
#include "Feature.h"
#include "Metric.h"
#include "N3L.h"

using namespace nr;
using namespace std;
using namespace mshadow;
using namespace mshadow::expr;
using namespace mshadow::utils;

//A native neural network classfier using only word embeddings
template<typename xpu>
class SparseRNNCRFMLClassifier {
public:
  SparseRNNCRFMLClassifier() {
    _dropOut = 0.5;
  }
  ~SparseRNNCRFMLClassifier() {

  }

public:
  LookupTable<xpu> _words;
  LookupTable<xpu> _chars;

  int _wordcontext, _wordwindow;
  int _wordSize;
  int _wordDim;

  int _charcontext, _charwindow;
  int _charSize;
  int _charDim;
  int _char_outputSize;
  int _char_inputSize;

  int _rnnhiddensize;
  int _hiddensize;
  int _inputsize, _token_representation_size;
  UniLayer<xpu> _tanh_project; 
  UniLayer<xpu> _olayer_linear;
  UniLayer<xpu> _tanhchar_project;
  AttentionPooling<xpu> _gatedchar_pooling;
  RNN<xpu> rnn_left_project;
  RNN<xpu> rnn_right_project;

  int _labelSize;

  Metric _eval;

  dtype _dropOut;

// add sparse and crf
  SparseUniLayer<xpu> _sparselayer_linear;
  int _linearfeatSize;
  MLCRFLoss<xpu> _crf_layer;

public:

  inline void init(const NRMat<dtype>& wordEmb, int wordcontext, const NRMat<dtype>& charEmb, int charcontext, int labelSize, int charhiddensize,
      int rnnhiddensize, int hiddensize, int linearfeatSize) {
    _wordcontext = wordcontext;
    _wordwindow = 2 * _wordcontext + 1;
    _wordSize = wordEmb.nrows();
    _wordDim = wordEmb.ncols();

    _charcontext = charcontext;
    _charwindow = 2 * _charcontext + 1;
    _charSize = charEmb.nrows();
    _charDim = charEmb.ncols();

    _char_inputSize = _charwindow * _charDim;
    _char_outputSize = charhiddensize;

    _labelSize = labelSize;
    _hiddensize = hiddensize;
    _rnnhiddensize = rnnhiddensize;
    _token_representation_size = _wordDim + _char_outputSize;
    _inputsize = _wordwindow * _token_representation_size;

    _words.initial(wordEmb);
    _chars.initial(charEmb);   

    rnn_left_project.initial(_rnnhiddensize, _inputsize, true, 20);
    rnn_right_project.initial(_rnnhiddensize, _inputsize, false, 30);
    _gatedchar_pooling.initial(_char_outputSize, _wordDim, true, 40);
    _tanhchar_project.initial(_char_outputSize, _char_inputSize, true, 50, 0);
    _tanh_project.initial(_hiddensize, 2 * _rnnhiddensize, true, 55, 0);
    _olayer_linear.initial(_labelSize, _hiddensize, false, 60, 2);
// add sparse and crf
    _linearfeatSize = linearfeatSize;
    _sparselayer_linear.initial(_labelSize, _linearfeatSize, false, 1000, 2);
    _crf_layer.initial(_labelSize, 70);
  }

  inline void release() {
    _words.release();
    _chars.release();
    _olayer_linear.release();
    _tanh_project.release();
    _tanhchar_project.release();
    _gatedchar_pooling.release();
    rnn_left_project.release();
    rnn_right_project.release();
    _sparselayer_linear.release();
    _crf_layer.release();
  }

  inline dtype process(const vector<Example>& examples, int iter) {
    _eval.reset();

    int example_num = examples.size();
    dtype cost = 0.0;
    int offset = 0;
    for (int count = 0; count < example_num; count++) {
      const Example& example = examples[count];

      int seq_size = example.m_features.size();
// add sparse
      vector<vector<int> > linear_features(seq_size);
      vector<Tensor<xpu, 2, dtype> > denseout(seq_size), denseoutLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > sparseout(seq_size), sparseoutLoss(seq_size);

      vector<Tensor<xpu, 2, dtype> > input(seq_size), inputLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > rnnoutput(seq_size), rnnoutputLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > project(seq_size), projectLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > output(seq_size), outputLoss(seq_size);

      vector<Tensor<xpu, 3, dtype> > charprime(seq_size), charprimeLoss(seq_size), charprimeMask(seq_size);
      vector<Tensor<xpu, 3, dtype> > charinput(seq_size), charinputLoss(seq_size);
      vector<Tensor<xpu, 3, dtype> > charhidden(seq_size), charhiddenLoss(seq_size);
      vector<Tensor<xpu, 3, dtype> > chargatedpoolIndex(seq_size), chargateweight(seq_size), chargateweightMiddle(seq_size);
      vector<Tensor<xpu, 2, dtype> > chargatedpool(seq_size), chargatedpoolLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > chargateweightsum(seq_size);
      vector<Tensor<xpu, 2, dtype> > wordprime(seq_size), wordprimeLoss(seq_size), wordprimeMask(seq_size);
      vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size), wordrepresentLoss(seq_size);
//add right-left content
      vector<Tensor<xpu, 2, dtype> > left_project(seq_size), left_projectLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > right_project(seq_size), right_projectLoss(seq_size);

      //initialize
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];

        int char_num = feature.chars.size();
        charprime[idx] = NewTensor<xpu>(Shape3(char_num, 1, _charDim), d_zero);
        charprimeLoss[idx] = NewTensor<xpu>(Shape3(char_num, 1, _charDim), d_zero);
        charprimeMask[idx] = NewTensor<xpu>(Shape3(char_num, 1, _charDim), d_one);
        charinput[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_inputSize), d_zero);
        charinputLoss[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_inputSize), d_zero);
        charhidden[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
        charhiddenLoss[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);

        chargatedpoolIndex[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
        chargateweightMiddle[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
        chargateweight[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
        chargateweightsum[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);
        chargatedpool[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);
        chargatedpoolLoss[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);

        wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
        wordprimeLoss[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
        wordprimeMask[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_one);
        wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        wordrepresentLoss[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
        inputLoss[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
        rnnoutput[idx] = NewTensor<xpu>(Shape2(1, 2 * _rnnhiddensize), d_zero);
        rnnoutputLoss[idx] = NewTensor<xpu>(Shape2(1, 2 * _rnnhiddensize), d_zero);        
        project[idx] = NewTensor<xpu>(Shape2(1, _hiddensize), d_zero);
        projectLoss[idx] = NewTensor<xpu>(Shape2(1, _hiddensize), d_zero);
        output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
        outputLoss[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
//add right-left content
        left_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
        left_projectLoss[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
        right_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
        right_projectLoss[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
// add sparse
        sparseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
        sparseoutLoss[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
        denseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
        denseoutLoss[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);

      }

      //forward propagation
      //input setting, and linear setting
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];
        //linear features should not be dropped out

        srand(iter * example_num + count * seq_size + idx);

        // add sparse features
        linear_features[idx].clear();
        for (int idy = 0; idy < feature.linear_features.size(); idy++) {
          if (1.0 * rand() / RAND_MAX >= _dropOut) {
            linear_features[idx].push_back(feature.linear_features[idy]);
          }
        }

        const vector<int>& words = feature.words;
        _words.GetEmb(words[0], wordprime[idx]);

        dropoutcol(wordprimeMask[idx], _dropOut);
        wordprime[idx] = wordprime[idx] * wordprimeMask[idx];

        const vector<int>& chars = feature.chars;
        int char_num = chars.size();

        //charprime
        for (int idy = 0; idy < char_num; idy++) {
          _chars.GetEmb(chars[idy], charprime[idx][idy]);
        }

        //char dropout
        for (int idy = 0; idy < char_num; idy++) {
          dropoutcol(charprimeMask[idx][idy], _dropOut);
          charprime[idx][idy] = charprime[idx][idy] * charprimeMask[idx][idy];
        }

        // char context
        windowlized(charprime[idx], charinput[idx], _charcontext);

        // char combination
        _tanhchar_project.ComputeForwardScore(charinput[idx], charhidden[idx]);

        // char gated pooling
        _gatedchar_pooling.ComputeForwardScore(charhidden[idx], wordprime[idx], chargateweightMiddle[idx], chargateweight[idx], chargateweightsum[idx],
            chargatedpoolIndex[idx], chargatedpool[idx]);
      }

      for (int idx = 0; idx < seq_size; idx++) {
        concat(wordprime[idx], chargatedpool[idx], wordrepresent[idx]);
      }

      windowlized(wordrepresent, input, _wordcontext);
      rnn_left_project.ComputeForwardScore(input, left_project);
      rnn_right_project.ComputeForwardScore(input, right_project);
      for (int idx = 0; idx < seq_size; idx++) {
        concat(left_project[idx], right_project[idx], rnnoutput[idx]);
      }
      _tanh_project.ComputeForwardScore(rnnoutput, project);
      _olayer_linear.ComputeForwardScore(project, denseout);

// add sparse
      _sparselayer_linear.ComputeForwardScore(linear_features, sparseout);
      for (int idx = 0; idx < seq_size; idx++) {
        output[idx] = denseout[idx] + sparseout[idx];
      }

      // get delta for each output
      cost += _crf_layer.loss(output, example.m_labels, outputLoss, _eval, example_num);

      // loss backward propagation
      // output
      _olayer_linear.ComputeBackwardLoss(project, denseout, outputLoss, projectLoss);
      _sparselayer_linear.ComputeBackwardLoss(linear_features, sparseout, outputLoss);

      _tanh_project.ComputeBackwardLoss(rnnoutput, project, projectLoss, rnnoutputLoss);
      for (int idx = 0; idx < seq_size; idx++) {
        unconcat(left_projectLoss[idx], right_projectLoss[idx], rnnoutputLoss[idx]);
      }

      // word combination
      rnn_left_project.ComputeBackwardLoss(input, left_project, left_projectLoss, inputLoss);
      rnn_right_project.ComputeBackwardLoss(input, right_project, right_projectLoss, inputLoss);

      // word context
      windowlized_backward(wordrepresentLoss, inputLoss, _wordcontext);

      // decompose loss
      for (int idx = 0; idx < seq_size; idx++) {
        unconcat(wordprimeLoss[idx], chargatedpoolLoss[idx], wordrepresentLoss[idx]);
      }

      for (int idx = 0; idx < seq_size; idx++) {
        _gatedchar_pooling.ComputeBackwardLoss(charhidden[idx], wordprime[idx], chargateweightMiddle[idx], chargateweight[idx], chargateweightsum[idx],
            chargatedpoolIndex[idx], chargatedpool[idx], chargatedpoolLoss[idx], charhiddenLoss[idx], wordprimeLoss[idx]);

        //char convolution
        _tanhchar_project.ComputeBackwardLoss(charinput[idx], charhidden[idx], charhiddenLoss[idx], charinputLoss[idx]);

        //char context
        windowlized_backward(charprimeLoss[idx], charinputLoss[idx], _charcontext);
      }

      // word fine tune
      if (_words.bEmbFineTune()) {
        for (int idx = 0; idx < seq_size; idx++) {
          const Feature& feature = example.m_features[idx];
          const vector<int>& words = feature.words;
          wordprimeLoss[idx] = wordprimeLoss[idx] * wordprimeMask[idx];
          _words.EmbLoss(words[0], wordprimeLoss[idx]);
        }
      }

      //char finetune
      if (_chars.bEmbFineTune()) {
        for (int idx = 0; idx < seq_size; idx++) {
          const Feature& feature = example.m_features[idx];
          const vector<int>& chars = feature.chars;
          int char_num = chars.size();
          for (int idy = 0; idy < char_num; idy++) {
            charprimeLoss[idx][idy] = charprimeLoss[idx][idy] * charprimeMask[idx][idy];
            _chars.EmbLoss(chars[idy], charprimeLoss[idx][idy]);
          }
        }
      }

      //release
      for (int idx = 0; idx < seq_size; idx++) {
        FreeSpace(&(charprime[idx]));
        FreeSpace(&(charprimeLoss[idx]));
        FreeSpace(&(charprimeMask[idx]));
        FreeSpace(&(charinput[idx]));
        FreeSpace(&(charinputLoss[idx]));
        FreeSpace(&(charhidden[idx]));
        FreeSpace(&(charhiddenLoss[idx]));
        FreeSpace(&(chargatedpoolIndex[idx]));
        FreeSpace(&(chargateweightMiddle[idx]));
        FreeSpace(&(chargateweight[idx]));
        FreeSpace(&(chargateweightsum[idx]));
        FreeSpace(&(chargatedpool[idx]));
        FreeSpace(&(chargatedpoolLoss[idx]));

        FreeSpace(&(wordprime[idx]));
        FreeSpace(&(wordprimeLoss[idx]));
        FreeSpace(&(wordprimeMask[idx]));
        FreeSpace(&(wordrepresent[idx]));
        FreeSpace(&(wordrepresentLoss[idx]));

        FreeSpace(&(input[idx]));
      FreeSpace(&(rnnoutput[idx]));
        FreeSpace(&(inputLoss[idx]));
        FreeSpace(&(rnnoutputLoss[idx]));
        FreeSpace(&(project[idx]));
        FreeSpace(&(projectLoss[idx]));
        FreeSpace(&(output[idx]));
        FreeSpace(&(outputLoss[idx]));
//add right-left content
        FreeSpace(&(left_project[idx]));
        FreeSpace(&(left_projectLoss[idx]));
        FreeSpace(&(right_project[idx]));
        FreeSpace(&(right_projectLoss[idx]));
// add sparse
        FreeSpace(&(sparseout[idx]));
        FreeSpace(&(sparseoutLoss[idx]));
        FreeSpace(&(denseout[idx]));
        FreeSpace(&(denseoutLoss[idx]));
      }
    }

    if (_eval.getAccuracy() < 0) {
      std::cout << "strange" << std::endl;
    }

    return cost;
  }

  void predict(const vector<Feature>& features, vector<int>& results) {
    int seq_size = features.size();
    int offset = 0;

    vector<Tensor<xpu, 2, dtype> > input(seq_size);
    vector<Tensor<xpu, 2, dtype> > rnnoutput(seq_size);
    vector<Tensor<xpu, 2, dtype> > project(seq_size);
    vector<Tensor<xpu, 2, dtype> > output(seq_size);
    // add right-left content
    vector<Tensor<xpu, 2, dtype> > left_project(seq_size);
    vector<Tensor<xpu, 2, dtype> > right_project(seq_size);
    // add sparse
    vector<Tensor<xpu, 2, dtype> > denseout(seq_size);
    vector<Tensor<xpu, 2, dtype> > sparseout(seq_size);

    vector<Tensor<xpu, 3, dtype> > charprime(seq_size);
    vector<Tensor<xpu, 3, dtype> > charinput(seq_size);
    vector<Tensor<xpu, 3, dtype> > charhidden(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargatedpoolIndex(seq_size);
    vector<Tensor<xpu, 2, dtype> > chargatedpool(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargateweightMiddle(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargateweight(seq_size);
    vector<Tensor<xpu, 2, dtype> > chargateweightsum(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size);

    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];

      int char_num = feature.chars.size();
      charprime[idx] = NewTensor<xpu>(Shape3(char_num, 1, _charDim), d_zero);
      charinput[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_inputSize), d_zero);
      charhidden[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargatedpoolIndex[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargatedpool[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);
      chargateweightMiddle[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargateweight[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargateweightsum[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);

      wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
      wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
      input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
      rnnoutput[idx] = NewTensor<xpu>(Shape2(1, 2 * _rnnhiddensize), d_zero);
      project[idx] = NewTensor<xpu>(Shape2(1, _hiddensize), d_zero);

      output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);

      //add right-left content
      left_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
      right_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);

            // add sparse
      sparseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
      denseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
    }

    //forward propagation
    //input setting, and linear setting
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];
      //linear features should not be dropped out
            // add sparse features
      _sparselayer_linear.ComputeForwardScore(feature.linear_features, sparseout[idx]);

      const vector<int>& words = feature.words;
      _words.GetEmb(words[0], wordprime[idx]);

      const vector<int>& chars = feature.chars;
      int char_num = chars.size();

      //charprime
      for (int idy = 0; idy < char_num; idy++) {
        _chars.GetEmb(chars[idy], charprime[idx][idy]);
      }

      // char context
      windowlized(charprime[idx], charinput[idx], _charcontext);

      // char combination
      _tanhchar_project.ComputeForwardScore(charinput[idx], charhidden[idx]);

      // char gated pooling
      _gatedchar_pooling.ComputeForwardScore(charhidden[idx], wordprime[idx], chargateweightMiddle[idx], chargateweight[idx], chargateweightsum[idx],
          chargatedpoolIndex[idx], chargatedpool[idx]);
    }

    for (int idx = 0; idx < seq_size; idx++) {
      concat(wordprime[idx], chargatedpool[idx], wordrepresent[idx]);
    }

    windowlized(wordrepresent, input, _wordcontext);
//add right-left content    
    rnn_left_project.ComputeForwardScore(input, left_project);
    rnn_right_project.ComputeForwardScore(input, right_project);
    for (int idx = 0; idx < seq_size; idx++) {
      concat(left_project[idx], right_project[idx], rnnoutput[idx]);
    }
    _tanh_project.ComputeForwardScore(rnnoutput, project);

    _olayer_linear.ComputeForwardScore(project, denseout);
// add sparse
    for (int idx = 0; idx < seq_size; idx++) {
      output[idx] = denseout[idx] + sparseout[idx];
    }

    // decode algorithm
    _crf_layer.predict(output, results);

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      FreeSpace(&(charprime[idx]));
      FreeSpace(&(charinput[idx]));
      FreeSpace(&(charhidden[idx]));
      FreeSpace(&(chargatedpoolIndex[idx]));
      FreeSpace(&(chargatedpool[idx]));
      FreeSpace(&(chargateweightMiddle[idx]));
      FreeSpace(&(chargateweight[idx]));
      FreeSpace(&(chargateweightsum[idx]));
      FreeSpace(&(wordprime[idx]));
      FreeSpace(&(wordrepresent[idx]));
      FreeSpace(&(input[idx]));
      FreeSpace(&(rnnoutput[idx]));
      FreeSpace(&(project[idx]));
      FreeSpace(&(output[idx]));
// add right-left content
      FreeSpace(&(left_project[idx]));
      FreeSpace(&(right_project[idx]));
      // add sparse
      FreeSpace(&(sparseout[idx]));
      FreeSpace(&(denseout[idx]));
    }
  }

  dtype computeScore(const Example& example) {
    int seq_size = example.m_features.size();
    int offset = 0;
    // add sparse
    vector<Tensor<xpu, 2, dtype> > denseout(seq_size);
    vector<Tensor<xpu, 2, dtype> > sparseout(seq_size);

    vector<Tensor<xpu, 2, dtype> > input(seq_size);
    vector<Tensor<xpu, 2, dtype> > rnnoutput(seq_size);
    vector<Tensor<xpu, 2, dtype> > project(seq_size);
    vector<Tensor<xpu, 2, dtype> > output(seq_size);
    // add right-left content
    vector<Tensor<xpu, 2, dtype> > left_project(seq_size);
    vector<Tensor<xpu, 2, dtype> > right_project(seq_size);

    vector<Tensor<xpu, 3, dtype> > charprime(seq_size);
    vector<Tensor<xpu, 3, dtype> > charinput(seq_size);
    vector<Tensor<xpu, 3, dtype> > charhidden(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargatedpoolIndex(seq_size);
    vector<Tensor<xpu, 2, dtype> > chargatedpool(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargateweightMiddle(seq_size);
    vector<Tensor<xpu, 3, dtype> > chargateweight(seq_size);
    vector<Tensor<xpu, 2, dtype> > chargateweightsum(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size);

    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = example.m_features[idx];

      int char_num = feature.chars.size();
      charprime[idx] = NewTensor<xpu>(Shape3(char_num, 1, _charDim), d_zero);
      charinput[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_inputSize), d_zero);
      charhidden[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargatedpoolIndex[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargatedpool[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);
      chargateweightMiddle[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargateweight[idx] = NewTensor<xpu>(Shape3(char_num, 1, _char_outputSize), d_zero);
      chargateweightsum[idx] = NewTensor<xpu>(Shape2(1, _char_outputSize), d_zero);

      wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
      wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

      input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
      rnnoutput[idx] = NewTensor<xpu>(Shape2(1, 2 * _rnnhiddensize), d_zero);
      project[idx] = NewTensor<xpu>(Shape2(1, _hiddensize), d_zero);

      output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
//add right-left content
      left_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
      right_project[idx] = NewTensor<xpu>(Shape2(1, _rnnhiddensize), d_zero);
      // add sparse
      sparseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
      denseout[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
    }

    //forward propagation
    //input setting, and linear setting
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = example.m_features[idx];
      //linear features should not be dropped out
            // add sparse features
      _sparselayer_linear.ComputeForwardScore(feature.linear_features, sparseout[idx]);

      const vector<int>& words = feature.words;
      _words.GetEmb(words[0], wordprime[idx]);

      const vector<int>& chars = feature.chars;
      int char_num = chars.size();

      //charprime
      for (int idy = 0; idy < char_num; idy++) {
        _chars.GetEmb(chars[idy], charprime[idx][idy]);
      }

      // char context
      windowlized(charprime[idx], charinput[idx], _charcontext);

      // char combination
      _tanhchar_project.ComputeForwardScore(charinput[idx], charhidden[idx]);

      // char gated pooling
      _gatedchar_pooling.ComputeForwardScore(charhidden[idx], wordprime[idx], chargateweightMiddle[idx], chargateweight[idx], chargateweightsum[idx],
          chargatedpoolIndex[idx], chargatedpool[idx]);
    }

    for (int idx = 0; idx < seq_size; idx++) {
      concat(wordprime[idx], chargatedpool[idx], wordrepresent[idx]);
    }

    windowlized(wordrepresent, input, _wordcontext);
    //add left-right content
    rnn_left_project.ComputeForwardScore(input, left_project);
    rnn_right_project.ComputeForwardScore(input, right_project);
    for (int idx = 0; idx < seq_size; idx++) {
      concat(left_project[idx], right_project[idx], rnnoutput[idx]);
    }
    _tanh_project.ComputeForwardScore(rnnoutput, project);


    _olayer_linear.ComputeForwardScore(project, denseout);
    // add sparse
    for (int idx = 0; idx < seq_size; idx++) {
      output[idx] = denseout[idx] + sparseout[idx];
    }

    // get delta for each output
    dtype cost = _crf_layer.cost(output, example.m_labels);

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      FreeSpace(&(charprime[idx]));
      FreeSpace(&(charinput[idx]));
      FreeSpace(&(charhidden[idx]));
      FreeSpace(&(chargatedpoolIndex[idx]));
      FreeSpace(&(chargatedpool[idx]));
      FreeSpace(&(chargateweightMiddle[idx]));
      FreeSpace(&(chargateweight[idx]));
      FreeSpace(&(chargateweightsum[idx]));
      FreeSpace(&(wordprime[idx]));
      FreeSpace(&(wordrepresent[idx]));
      FreeSpace(&(input[idx]));
      FreeSpace(&(rnnoutput[idx]));
      FreeSpace(&(project[idx]));
      FreeSpace(&(output[idx]));
//add right-left content
      FreeSpace(&(left_project[idx]));
      FreeSpace(&(right_project[idx]));
      // add sparse
      FreeSpace(&(sparseout[idx]));
      FreeSpace(&(denseout[idx]));
    }
    return cost;
  }

  void updateParams(dtype nnRegular, dtype adaAlpha, dtype adaEps) {
    rnn_left_project.updateAdaGrad(nnRegular, adaAlpha, adaEps);
//add right-left content
    rnn_right_project.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    // add sparse and crf
    _sparselayer_linear.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _crf_layer.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _tanhchar_project.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _gatedchar_pooling.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _tanh_project.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _olayer_linear.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _words.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _chars.updateAdaGrad(nnRegular, adaAlpha, adaEps);    
  }

  void writeModel();

  void loadModel();

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    for (int i = 0; i < Wd.size(0); ++i)
      idRows.push_back(i);
    for (int idx = 0; idx < Wd.size(1); idx++)
      idCols.push_back(idx);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;
  }

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter,
      const hash_set<int>& indexes, bool bRow = true) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    static hash_set<int>::iterator it;
    if (bRow) {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idRows.push_back(*it);
      for (int idx = 0; idx < Wd.size(1); idx++)
        idCols.push_back(idx);
    } else {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idCols.push_back(*it);
      for (int idx = 0; idx < Wd.size(0); idx++)
        idRows.push_back(idx);
    }

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;

  }

  void checkgrads(const vector<Example>& examples, int iter) {
    checkgrad(examples, _olayer_linear._W, _olayer_linear._gradW, "_olayer_linear._W", iter);
    checkgrad(examples, _tanh_project._W, _tanh_project._gradW, "_tanh_project._W", iter);

    checkgrad(examples, _tanhchar_project._W, _tanhchar_project._gradW, "_tanhchar_project._W", iter);
    checkgrad(examples, _tanhchar_project._b, _tanhchar_project._gradb, "_tanhchar_project._b", iter);

    checkgrad(examples, _gatedchar_pooling._bi_gates._WL, _gatedchar_pooling._bi_gates._gradWL, "_gatedchar_pooling._bi_gates._WL", iter);
    checkgrad(examples, _gatedchar_pooling._bi_gates._WR, _gatedchar_pooling._bi_gates._gradWR, "_gatedchar_pooling._bi_gates._WR", iter);
    checkgrad(examples, _gatedchar_pooling._bi_gates._b, _gatedchar_pooling._bi_gates._gradb, "_gatedchar_pooling._bi_gates._b", iter);

    checkgrad(examples, _gatedchar_pooling._uni_gates._W, _gatedchar_pooling._uni_gates._gradW, "_gatedchar_pooling._uni_gates._W", iter);
    checkgrad(examples, _gatedchar_pooling._uni_gates._b, _gatedchar_pooling._uni_gates._gradb, "_gatedchar_pooling._uni_gates._b", iter);

    checkgrad(examples, rnn_left_project._rnn._WL, rnn_left_project._rnn._gradWL, "rnn_left_project._rnn._WL", iter);
    checkgrad(examples, rnn_left_project._rnn._WR, rnn_left_project._rnn._gradWR, "rnn_left_project._rnn._WR", iter);
    checkgrad(examples, rnn_left_project._rnn._b, rnn_left_project._rnn._gradb, "rnn_left_project._rnn._b", iter);
//add right-left content
    checkgrad(examples, rnn_right_project._rnn._WL, rnn_right_project._rnn._gradWL, "rnn_right_project._rnn._WL", iter);
    checkgrad(examples, rnn_right_project._rnn._WR, rnn_right_project._rnn._gradWR, "rnn_right_project._rnn._WR", iter);
    checkgrad(examples, rnn_right_project._rnn._b, rnn_right_project._rnn._gradb, "rnn_right_project._rnn._b", iter);
    // add sparse and crf
    checkgrad(examples, _sparselayer_linear._W, _sparselayer_linear._gradW, "_sparselayer_linear._W", iter, _sparselayer_linear._indexers, false);
    checkgrad(examples, _crf_layer._tagBigram, _crf_layer._grad_tagBigram, "_crf_layer._tagBigram", iter);

    checkgrad(examples, _words._E, _words._gradE, "_words._E", iter, _words._indexers);
    checkgrad(examples, _chars._E, _chars._gradE, "_chars._E", iter, _chars._indexers);

  }

public:
  inline void resetEval() {
    _eval.reset();
  }

  inline void setDropValue(dtype dropOut) {
    _dropOut = dropOut;
  }

  inline void setWordEmbFinetune(bool b_wordEmb_finetune) {
    _words.setEmbFineTune(b_wordEmb_finetune);
  }

};

#endif /* SRC_SparseRNNCRFMLClassifier_H_ */
