/**
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 * @format
 */
import React from 'react';
import {AppRegistry, Text, View} from 'react-native';

export default class Bootstrap extends React.Component {
  render() {
    return (
      <View
        accessible={true}
        accessibilityValue={{now: 10, min: 0, max: 20}}
        style={{borderRadius: 30, width: 60, height: 60, margin: 10}}>
        <View style={{backgroundColor: 'magenta', width: 60, height: 60}} />
        <Text>Hello World</Text>
      </View>
    );
  }
}

AppRegistry.registerComponent('Bootstrap', () => Bootstrap);
